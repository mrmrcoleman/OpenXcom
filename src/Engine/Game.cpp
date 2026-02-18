/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "Game.h"
#include "SDL2_compat.h"
#include "../resource.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <SDL_mixer.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif
#include "State.h"
#include "Screen.h"
#include "Sound.h"
#include "Music.h"
#include "Language.h"
#include "Logger.h"
#include "../Interface/Cursor.h"
#include "../Interface/FpsCounter.h"
#include "../Mod/Mod.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/SavedBattleGame.h"
#include "Action.h"
#include "Exception.h"
#include "Options.h"
#include "CrossPlatform.h"
#include "FileMap.h"
#include "Unicode.h"
#include "../Menu/TestState.h"
#ifdef __EMSCRIPTEN__
#include "../Battlescape/BattlescapeState.h"
#endif

namespace OpenXcom
{

#ifdef __EMSCRIPTEN__
/**
 * Emscripten HTML5 mouse callback.
 *
 * The Emscripten SDL2 port does not reliably generate SDL mouse events
 * when using the SDL 1.2 → SDL2 compatibility shim (SDL_SetVideoMode).
 * Work around this by registering our own mouse callbacks via the
 * Emscripten HTML5 API and pushing synthetic SDL events.
 */
/*
 * Exported C functions called directly from JavaScript mouse event listeners.
 * This is the most reliable approach: JS listeners on the canvas call these
 * functions via Module._pushMouseMove / _pushMouseButton, which push
 * synthetic SDL events into the queue.
 */
/* Button mask tracked by pushMouseButton so that OX_GetMouseButtonState()
 * (see EmMouseState.h) returns the correct state. SDL_GetMouseState()
 * does NOT reflect synthetic events pushed via SDL_PushEvent. */
static unsigned int s_emMouseButtons = 0;

extern "C" {

EMSCRIPTEN_KEEPALIVE
unsigned int emGetMouseButtons() { return s_emMouseButtons; }

EMSCRIPTEN_KEEPALIVE
void pushMouseMove(int x, int y, int movX, int movY, int buttons)
{
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = SDL_MOUSEMOTION;
	ev.motion.x = x;
	ev.motion.y = y;
	ev.motion.xrel = movX;
	ev.motion.yrel = movY;
	ev.motion.state = buttons;
	SDL_PushEvent(&ev);
}

EMSCRIPTEN_KEEPALIVE
void pushMouseButton(int down, int x, int y, int button)
{
	/* Update our own button mask (SDL_BUTTON(n) = 1 << (n-1)). */
	if (button >= 1 && button <= 5)
	{
		unsigned int bit = 1u << (button - 1);
		if (down) s_emMouseButtons |= bit;
		else      s_emMouseButtons &= ~bit;
	}

	SDL_Event ev;
	SDL_zero(ev);
	ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	ev.button.x = x;
	ev.button.y = y;
	ev.button.button = button;
	ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.button.clicks = 1;
	SDL_PushEvent(&ev);

	/* Emscripten SDL may not update internal mouse position from button events.
	 * On state init the game does SDL_GetMouseState() and uses that for a synthetic
	 * motion event; if it gets (0,0) we get spurious edge-scroll. Push a motion
	 * event so SDL's position stays correct. */
	ev.type = SDL_MOUSEMOTION;
	ev.motion.x = x;
	ev.motion.y = y;
	ev.motion.xrel = 0;
	ev.motion.yrel = 0;
	ev.motion.state = s_emMouseButtons;
	SDL_PushEvent(&ev);
}

EMSCRIPTEN_KEEPALIVE
void pushMouseWheel(int x, int y, int deltaX, int deltaY)
{
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = SDL_MOUSEWHEEL;
	ev.wheel.x = deltaX;
	ev.wheel.y = deltaY;
	ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
	SDL_PushEvent(&ev);
}

} /* extern "C" */
#endif

const double Game::VOLUME_GRADIENT = 10.0;

/**
 * Starts up all the SDL subsystems,
 * creates the display screen and sets up the cursor.
 * @param title Title of the game window.
 */
Game::Game(const std::string &title) : _screen(0), _cursor(0), _lang(0), _save(0), _mod(0), _quit(false), _init(false), _mouseActive(true), _timeUntilNextFrame(0)
{
	Options::reload = false;
	Options::mute = false;

	// Initialize SDL
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		throw Exception(SDL_GetError());
	}
	Log(LOG_INFO) << "SDL initialized successfully.";

#ifdef __EMSCRIPTEN__
	/* Register mouse event listeners directly in JavaScript.
	 * The Emscripten SDL2 port does not reliably forward browser mouse
	 * events to the SDL event queue when using our SDL 1.2 compat shim.
	 * We attach JS listeners on the canvas that call our exported C
	 * functions (pushMouseMove / pushMouseButton) to inject SDL events. */
	EM_ASM({
		var canvas = document.getElementById('canvas');
		if (!canvas) { console.error('WASM mouse: #canvas not found'); return; }

		/* Debug logging: add ?inputdebug to URL, or set window.OPENXCOM_INPUT_DEBUG=true before starting */
		var _inputDebug = (window.location.search.indexOf('inputdebug') !== -1) || (window.OPENXCOM_INPUT_DEBUG === true);

		/* Track when the last mousedown happened so we can suppress
		   wheel noise generated by the trackpad physical click. */
		var _lastMouseDownTime = 0;
		var CLICK_COOLDOWN_MS = 150;
		/* Track mousedown position so we can snap mouseup for tiny movements (trackpad click jitter). */
		var _lastMouseDownX = -1;
		var _lastMouseDownY = -1;
		var _clickJitterThresholdPx = 8;
		/* After a click, the game may call SDL_WarpMouse(); in the browser that can make the next
		   position appear at the screen edge and trigger edge-scroll. Track mouseup and filter that. */
		var _lastMouseUpTime = 0;
		var _lastMouseUpX = -1;
		var _lastMouseUpY = -1;
		var _warpArtifactWindowMs = 200;
		var _edgeThresholdPx = 15;

		canvas.addEventListener('mousemove', function(e) {
			var r = canvas.getBoundingClientRect();
			var sx = canvas.width / r.width;
			var sy = canvas.height / r.height;
			var x = (e.clientX - r.left) * sx | 0;
			var y = (e.clientY - r.top)  * sy | 0;
			var moveX = e.movementX|0;
			var moveY = e.movementY|0;
			/* Suppress tiny movement during the first 200ms after mousedown (trackpad click jitter). */
			if (_lastMouseDownX >= 0 && (performance.now() - _lastMouseDownTime) < 200) {
				var dx = x - _lastMouseDownX;
				var dy = y - _lastMouseDownY;
				if (dx*dx + dy*dy <= _clickJitterThresholdPx * _clickJitterThresholdPx) {
					moveX = 0;
					moveY = 0;
				}
			}
			/* Filter warp artifact: game may call SDL_WarpMouse() after a click; the next position
			   can appear at the screen edge and trigger edge-scroll. If we see position at edge
			   shortly after mouseup and the click was not at the edge, treat as artifact and report
			   the click position with no movement. */
			if (_lastMouseUpX >= 0 && (performance.now() - _lastMouseUpTime) < _warpArtifactWindowMs) {
				var atEdge = (x < _edgeThresholdPx || x > canvas.width - _edgeThresholdPx ||
				              y < _edgeThresholdPx || y > canvas.height - _edgeThresholdPx);
				var clickWasAtEdge = (_lastMouseUpX < _edgeThresholdPx || _lastMouseUpX > canvas.width - _edgeThresholdPx ||
				                     _lastMouseUpY < _edgeThresholdPx || _lastMouseUpY > canvas.height - _edgeThresholdPx);
				if (atEdge && !clickWasAtEdge) {
					x = _lastMouseUpX;
					y = _lastMouseUpY;
					moveX = 0;
					moveY = 0;
					if (_inputDebug) console.log('[input] mousemove warp artifact filtered (was at edge after click)');
				}
			}
			Module._pushMouseMove(x, y, moveX, moveY, e.buttons|0);
		});
		canvas.addEventListener('mousedown', function(e) {
			_lastMouseDownTime = performance.now();
			if (_wheelDebounceTimer) { clearTimeout(_wheelDebounceTimer); _wheelDebounceTimer = null; }
			_wheelPending = [];
			var r = canvas.getBoundingClientRect();
			var sx = canvas.width / r.width;
			var sy = canvas.height / r.height;
			var x = (e.clientX - r.left) * sx | 0;
			var y = (e.clientY - r.top)  * sy | 0;
			_lastMouseDownX = x;
			_lastMouseDownY = y;
			if (_inputDebug) console.log('[input] mousedown btn=' + e.button + ' x=' + x + ' y=' + y);
			Module._pushMouseButton(1, x, y, e.button + 1);
		});
		canvas.addEventListener('mouseup', function(e) {
			var r = canvas.getBoundingClientRect();
			var sx = canvas.width / r.width;
			var sy = canvas.height / r.height;
			var x = (e.clientX - r.left) * sx | 0;
			var y = (e.clientY - r.top)  * sy | 0;
			if (_lastMouseDownX >= 0 && _lastMouseDownY >= 0) {
				var dx = x - _lastMouseDownX;
				var dy = y - _lastMouseDownY;
				if (dx*dx + dy*dy <= _clickJitterThresholdPx * _clickJitterThresholdPx) {
					x = _lastMouseDownX;
					y = _lastMouseDownY;
					if (_inputDebug) console.log('[input] mouseup snapped to mousedown (jitter filter)');
				}
			}
			_lastMouseDownX = -1;
			_lastMouseDownY = -1;
			_lastMouseUpTime = performance.now();
			_lastMouseUpX = x;
			_lastMouseUpY = y;
			if (_inputDebug) console.log('[input] mouseup btn=' + e.button + ' x=' + x + ' y=' + y);
			Module._pushMouseButton(0, x, y, e.button + 1);
		});
		/* Trackpad / Magic Mouse scroll → drag-scroll state */
		var _scrollDragging = false;
		var _scrollTimer = null;
		var _scrollLastX = 0;
		var _scrollLastY = 0;
		/* Debounce: delay applying trackpad wheel so we can cancel if user was actually clicking */
		var _wheelDebounceTimer = null;
		var _wheelDebounceMs = 100;
		var _wheelPending = [];  /* accumulated { x, y, moveX, moveY } until timer fires or mousedown cancels */

		canvas.addEventListener('wheel', function(e) {
			e.preventDefault();
			var r = canvas.getBoundingClientRect();
			var sx = canvas.width / r.width;
			var sy = canvas.height / r.height;
			var x = (e.clientX - r.left) * sx | 0;
			var y = (e.clientY - r.top)  * sy | 0;
			var sinceClick = performance.now() - _lastMouseDownTime;

			if (_inputDebug) {
				console.log('[input] wheel dX=' + e.deltaX.toFixed(1) +
					' dY=' + e.deltaY.toFixed(1) +
					' mode=' + e.deltaMode +
					' sinceClick=' + sinceClick.toFixed(0) + 'ms' +
					' ctrl=' + e.ctrlKey +
					(sinceClick < CLICK_COOLDOWN_MS ? ' SUPPRESSED' : ' ok'));
			}

			/* Suppress wheel events that fire within a short window
			   after a mousedown — these are noise from the trackpad
			   physical click, not intentional scrolling. */
			if (sinceClick < CLICK_COOLDOWN_MS) { return; }

			/* Pinch-to-zoom on trackpad (Chrome/Safari set ctrlKey
			   for trackpad pinch gestures) → zoom / level change. */
			if (e.ctrlKey) {
				var dy = e.deltaY < 0 ? -1 : 1;
				Module._pushMouseWheel(x, y, 0, dy);
				return;
			}

			/* Physical mouse wheel detection:
			   - Firefox uses deltaMode=1 (lines) for a real mouse wheel.
			   - Chrome/Safari use deltaMode=0 (pixels) but report ~100px
			     per wheel notch, whereas trackpad events are 1-30px.
			   A threshold of 50 cleanly separates the two. */
			var dominated = Math.abs(e.deltaY) > Math.abs(e.deltaX);
			if (e.deltaMode !== 0 ||
				(dominated && Math.abs(e.deltaY) >= 50 && Math.abs(e.deltaX) < 5)) {
				var dy = e.deltaY < 0 ? -1 : 1;
				Module._pushMouseWheel(x, y, 0, dy);
				return;
			}

			/* Trackpad / Magic Mouse finger stroke. Debounce: delay applying
			   so that if the user was actually clicking (mousedown follows
			   shortly), we cancel the scroll. Wheel-before-click is a common
			   trackpad false positive. Accumulate deltas until timer fires. */
			var moveX = Math.round(-e.deltaX * sx * 0.5);
			var moveY = Math.round(-e.deltaY * sy * 0.5);
			_wheelPending.push({ x: x, y: y, moveX: moveX, moveY: moveY });
			if (_wheelDebounceTimer) clearTimeout(_wheelDebounceTimer);
			_wheelDebounceTimer = setTimeout(function() {
				_wheelDebounceTimer = null;
				var list = _wheelPending;
				_wheelPending = [];
				if (list.length === 0) return;
				var totalX = 0, totalY = 0;
				for (var i = 0; i < list.length; i++) {
					totalX += list[i].moveX;
					totalY += list[i].moveY;
				}
				var first = list[0];
				var last = list[list.length - 1];
				if (!_scrollDragging) {
					_scrollDragging = true;
					_scrollLastX = first.x;
					_scrollLastY = first.y;
					Module._pushMouseButton(1, first.x, first.y, 3);
				}
				Module._pushMouseMove(last.x, last.y, totalX, totalY, 4);
				clearTimeout(_scrollTimer);
				_scrollTimer = setTimeout(function() {
					_scrollDragging = false;
					Module._pushMouseButton(0, _scrollLastX, _scrollLastY, 3);
				}, 120);
			}, _wheelDebounceMs);
		}, { passive: false });
		console.log('WASM mouse: JS listeners registered on canvas. Input debug: ' + (_inputDebug ? 'ON' : 'off (add ?inputdebug to URL or set OPENXCOM_INPUT_DEBUG=true)'));
	});
#endif

	// Initialize SDL_mixer
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		Log(LOG_ERROR) << SDL_GetError();
		Log(LOG_WARNING) << "No sound device detected, audio disabled.";
		Options::mute = true;
	}
	else
	{
		initAudio();
	}

	// trap the mouse inside the window
	SDL_WM_GrabInput(Options::captureMouse);
	
	// Set the window icon
	CrossPlatform::setWindowIcon(IDI_ICON1, FileMap::getFilePath("openxcom.png"));

	// Set the window caption
	SDL_WM_SetCaption(title.c_str(), 0);

	// Set up unicode
	SDL_EnableUNICODE(1);
	Unicode::getUtf8Locale();

	// Create display
	_screen = new Screen();

	// Create cursor
	_cursor = new Cursor(9, 13);
	
	// Hide the system cursor — the game draws its own cursor in the game buffer
	SDL_ShowCursor(SDL_DISABLE);

	// Create fps counter
	_fpsCounter = new FpsCounter(15, 5, 0, 0);

	// Create blank language
	_lang = new Language();

	_timeOfLastFrame = 0;
}

/**
 * Deletes the display screen, cursor, states and shuts down all the SDL subsystems.
 */
Game::~Game()
{
	Sound::stop();
	Music::stop();

	for (std::list<State*>::iterator i = _states.begin(); i != _states.end(); ++i)
	{
		delete *i;
	}

	SDL_FreeCursor(SDL_GetCursor());

	delete _cursor;
	delete _lang;
	delete _save;
	delete _mod;
	delete _screen;
	delete _fpsCounter;

	Mix_CloseAudio();

	SDL_Quit();
}

/**
 * The state machine takes care of passing all the events from SDL to the
 * active state, running any code within and blitting all the states and
 * cursor to the screen. This is run indefinitely until the game quits.
 */
void Game::run()
{
	enum ApplicationState { RUNNING = 0, SLOWED = 1, PAUSED = 2 } runningState = RUNNING;
	static const ApplicationState kbFocusRun[4] = { RUNNING, RUNNING, SLOWED, PAUSED };
	static const ApplicationState stateRun[4] = { SLOWED, PAUSED, PAUSED, PAUSED };
	// this will avoid processing SDL's resize event on startup, workaround for the heap allocation error it causes.
	bool startupEvent = Options::allowResize;
	while (!_quit)
	{
		// Clean up states
		while (!_deleted.empty())
		{
			delete _deleted.back();
			_deleted.pop_back();
		}

		// Initialize active state
		if (!_init)
		{
			_init = true;
			_states.back()->init();

			// Unpress buttons
			_states.back()->resetAll();

			// Refresh mouse position
			SDL_Event ev;
			int x, y;
#ifdef __EMSCRIPTEN__
			// In the browser, SDL_GetMouseState can be (0,0) when battlescape starts,
			// which triggers edge-scroll. Use screen center so battlescape starts with
			// no scroll; real cursor position will come from the next mousemove.
			if (dynamic_cast<BattlescapeState*>(_states.back()))
			{
				x = _screen->getWidth() / 2;
				y = _screen->getHeight() / 2;
				SDL_Event centerEv;
				SDL_zero(centerEv);
				centerEv.type = SDL_MOUSEMOTION;
				centerEv.motion.x = x;
				centerEv.motion.y = y;
				centerEv.motion.xrel = 0;
				centerEv.motion.yrel = 0;
				centerEv.motion.state = 0;
				SDL_PushEvent(&centerEv);
			}
			else
#endif
			SDL_GetMouseState(&x, &y);
			ev.type = SDL_MOUSEMOTION;
			ev.motion.x = x;
			ev.motion.y = y;
			Action action = Action(&ev, _screen->getXScale(), _screen->getYScale(), _screen->getCursorTopBlackBand(), _screen->getCursorLeftBlackBand());
			_states.back()->handle(&action);
		}

		// Process events
		while (SDL_PollEvent(&_event))
		{
			if (CrossPlatform::isQuitShortcut(_event))
				_event.type = SDL_QUIT;
			switch (_event.type)
			{
				case SDL_QUIT:
					quit();
					break;
			case SDL_WINDOWEVENT:
				switch (_event.window.event)
				{
				case SDL_WINDOWEVENT_MINIMIZED:
					runningState = stateRun[Options::pauseMode];
					if (Options::backgroundMute)
					{
						setVolume(0, 0, 0);
					}
					break;
				case SDL_WINDOWEVENT_FOCUS_LOST:
					runningState = kbFocusRun[Options::pauseMode];
					if (Options::backgroundMute)
					{
						setVolume(0, 0, 0);
					}
					break;
				case SDL_WINDOWEVENT_RESTORED:
				case SDL_WINDOWEVENT_FOCUS_GAINED:
					runningState = RUNNING;
					if (Options::backgroundMute)
					{
						setVolume(Options::soundVolume, Options::musicVolume, Options::uiVolume);
					}
					break;
				case SDL_WINDOWEVENT_RESIZED:
					if (Options::allowResize)
					{
						if (!startupEvent)
						{
							Options::newDisplayWidth = Options::displayWidth = std::max(Screen::ORIGINAL_WIDTH, (int)_event.window.data1);
							Options::newDisplayHeight = Options::displayHeight = std::max(Screen::ORIGINAL_HEIGHT, (int)_event.window.data2);
							int dX = 0, dY = 0;
							Screen::updateScale(Options::battlescapeScale, Options::baseXBattlescape, Options::baseYBattlescape, false);
							Screen::updateScale(Options::geoscapeScale, Options::baseXGeoscape, Options::baseYGeoscape, false);
							for (std::list<State*>::iterator i = _states.begin(); i != _states.end(); ++i)
							{
								(*i)->resize(dX, dY);
							}
							_screen->resetDisplay();
						}
						else
						{
							startupEvent = false;
						}
					}
					break;
				default:
					break;
				} /* end inner SDL_WINDOWEVENT switch */
				break;
			case SDL_MOUSEWHEEL:
				{
					/* SDL2 fires SDL_MOUSEWHEEL instead of button 4/5.
					 * Synthesize SDL_MOUSEBUTTONDOWN + UP events so all the
					 * existing SDL_BUTTON_WHEELUP / WHEELDOWN handlers work.
					 * We must send both DOWN and UP: InteractiveSurface marks
					 * the button as "pressed" on DOWN and blocks further events
					 * until an UP clears it. */
					if (_event.wheel.y == 0) { continue; }
					Uint8 btn = (_event.wheel.y > 0) ? SDL_BUTTON_WHEELUP : SDL_BUTTON_WHEELDOWN;
					int mx = 0, my = 0;
					SDL_GetMouseState(&mx, &my);
					int repeats = std::max(1, std::abs(_event.wheel.y));
					for (int wr = 0; wr < repeats; ++wr)
					{
						SDL_Event synthDown;
						SDL_zero(synthDown);
						synthDown.type = SDL_MOUSEBUTTONDOWN;
						synthDown.button.button = btn;
						synthDown.button.x = mx;
						synthDown.button.y = my;
						synthDown.button.clicks = 1;
						synthDown.button.state = SDL_PRESSED;
						SDL_PushEvent(&synthDown);

						SDL_Event synthUp;
						SDL_zero(synthUp);
						synthUp.type = SDL_MOUSEBUTTONUP;
						synthUp.button.button = btn;
						synthUp.button.x = mx;
						synthUp.button.y = my;
						synthUp.button.clicks = 1;
						synthUp.button.state = SDL_RELEASED;
						SDL_PushEvent(&synthUp);
					}
					continue;   /* skip default handling for the raw wheel event */
				}
			case SDL_MOUSEMOTION:
				case SDL_MOUSEBUTTONDOWN:
				case SDL_MOUSEBUTTONUP:
					// Skip mouse events if they're disabled
					if (!_mouseActive) continue;
					// re-gain focus on mouse-over or keypress.
					runningState = RUNNING;
					
					
					// Go on, feed the event to others
				default:
					Action action = Action(&_event, _screen->getXScale(), _screen->getYScale(), _screen->getCursorTopBlackBand(), _screen->getCursorLeftBlackBand());
					_screen->handle(&action);
					_cursor->handle(&action);
					_fpsCounter->handle(&action);
					if (action.getDetails()->type == SDL_KEYDOWN)
					{
						// "ctrl-g" grab input
						if (action.getDetails()->key.keysym.sym == SDLK_g && (SDL_GetModState() & KMOD_CTRL) != 0)
						{
							Options::captureMouse = (SDL_GrabMode)(!Options::captureMouse);
							SDL_WM_GrabInput(Options::captureMouse);
						}
						else if (Options::debug)
						{
							if (action.getDetails()->key.keysym.sym == SDLK_t && (SDL_GetModState() & KMOD_CTRL) != 0)
							{
								setState(new TestState);
							}
							// "ctrl-u" debug UI
							else if (action.getDetails()->key.keysym.sym == SDLK_u && (SDL_GetModState() & KMOD_CTRL) != 0)
							{
								Options::debugUi = !Options::debugUi;
								_states.back()->redrawText();
							}
						}
					}
					_states.back()->handle(&action);
					break;
			}
			if (!_init)
			{
				// States stack was changed, break the loop so new state
				// can be initialized before processing new events
				break;
			}
		}
		
		// Process rendering
		if (runningState != PAUSED)
		{
			// Process logic
			_states.back()->think();
			_fpsCounter->think();
			if (Options::FPS > 0 && !(Options::useOpenGL && Options::vSyncForOpenGL))
			{
				// Update our FPS delay time based on the time of the last draw.
				/* SDL2: check keyboard focus via window flags */
			SDL_Window *_focusWin = SDL2Compat::getWindow();
			Uint32 _winFlags = _focusWin ? SDL_GetWindowFlags(_focusWin) : 0;
			int fps = (_winFlags & SDL_WINDOW_INPUT_FOCUS) ? Options::FPS : Options::FPSInactive;

				_timeUntilNextFrame = (1000.0f / fps) - (SDL_GetTicks() - _timeOfLastFrame);
			}
			else
			{
				_timeUntilNextFrame = 0;
			}

			if (_init && _timeUntilNextFrame <= 0)
			{
				// make a note of when this frame update occurred.
				_timeOfLastFrame = SDL_GetTicks();
				_fpsCounter->addFrame();
				_screen->clear();
				std::list<State*>::iterator i = _states.end();
				do
				{
					--i;
				}
				while (i != _states.begin() && !(*i)->isScreen());

				for (; i != _states.end(); ++i)
				{
					(*i)->blit();
				}
				_fpsCounter->blit(_screen->getSurface());
				_cursor->blit(_screen->getSurface());
				_screen->flip();
			}
		}

		// Save on CPU
		switch (runningState)
		{
			case RUNNING: 
				SDL_Delay(1); //Save CPU from going 100%
				break;
			case SLOWED: case PAUSED:
				SDL_Delay(100); break; //More slowing down.
		}

#ifdef __EMSCRIPTEN__
		/* Periodically persist the IDBFS-backed filesystem to IndexedDB
		 * so that saves, options, etc. survive browser refreshes.
		 * Syncing every ~30 seconds avoids excessive I/O overhead. */
		{
			static Uint32 _lastIdbSync = 0;
			Uint32 now = SDL_GetTicks();
			if (now - _lastIdbSync > 30000)
			{
				_lastIdbSync = now;
				EM_ASM( if (typeof Module.FS !== 'undefined') Module.FS.syncfs(false, function(e){ if(e) console.error('IDBFS sync error',e); }); );
			}
		}
#endif
	}

	Options::save();
}

/**
 * Stops the state machine and the game is shut down.
 */
void Game::quit()
{
	// Always save ironman
	if (_save != 0 && _save->isIronman() && !_save->getName().empty())
	{
		std::string filename = CrossPlatform::sanitizeFilename(Unicode::convUtf8ToPath(_save->getName())) + ".sav";
		_save->save(filename);
	}
	_quit = true;
}

/**
 * Changes the audio volume of the music and
 * sound effect channels.
 * @param sound Sound volume, from 0 to MIX_MAX_VOLUME.
 * @param music Music volume, from 0 to MIX_MAX_VOLUME.
 * @param ui UI volume, from 0 to MIX_MAX_VOLUME.
 */
void Game::setVolume(int sound, int music, int ui)
{
	if (!Options::mute)
	{
		if (sound >= 0)
		{
			sound = volumeExponent(sound) * (double)SDL_MIX_MAXVOLUME;
			Mix_Volume(-1, sound);
			if (_save && _save->getSavedBattle())
			{
				Mix_Volume(3, sound * _save->getSavedBattle()->getAmbientVolume());
			}
			else
			{
				// channel 3: reserved for ambient sound effect.
				Mix_Volume(3, sound / 2);
			}
		}
		if (music >= 0)
		{
			music = volumeExponent(music) * (double)SDL_MIX_MAXVOLUME;
			Mix_VolumeMusic(music);
		}
		if (ui >= 0)
		{
			ui = volumeExponent(ui) * (double)SDL_MIX_MAXVOLUME;
			Mix_Volume(1, ui);
			Mix_Volume(2, ui);
		}
	}
}

double Game::volumeExponent(int volume)
{
	return (exp(log(Game::VOLUME_GRADIENT + 1.0) * volume / (double)SDL_MIX_MAXVOLUME) -1.0 ) / Game::VOLUME_GRADIENT;
}

/**
 * Returns the display screen used by the game.
 * @return Pointer to the screen.
 */
Screen *Game::getScreen() const
{
	return _screen;
}

/**
 * Returns the mouse cursor used by the game.
 * @return Pointer to the cursor.
 */
Cursor *Game::getCursor() const
{
	return _cursor;
}

/**
 * Returns the FpsCounter used by the game.
 * @return Pointer to the FpsCounter.
 */
FpsCounter *Game::getFpsCounter() const
{
	return _fpsCounter;
}

/**
 * Pops all the states currently in stack and pushes in the new state.
 * A shortcut for cleaning up all the old states when they're not necessary
 * like in one-way transitions.
 * @param state Pointer to the new state.
 */
void Game::setState(State *state)
{
	while (!_states.empty())
	{
		popState();
	}
	pushState(state);
	_init = false;
}

/**
 * Pushes a new state into the top of the stack and initializes it.
 * The new state will be used once the next game cycle starts.
 * @param state Pointer to the new state.
 */
void Game::pushState(State *state)
{
	_states.push_back(state);
	_init = false;
}

/**
 * Pops the last state from the top of the stack. Since states
 * can't actually be deleted mid-cycle, it's moved into a separate queue
 * which is cleared at the start of every cycle, so the transition
 * is seamless.
 */
void Game::popState()
{
	_deleted.push_back(_states.back());
	_states.pop_back();
	_init = false;
}

/**
 * Returns the language currently in use by the game.
 * @return Pointer to the language.
 */
Language *Game::getLanguage() const
{
	return _lang;
}

/**
 * Returns the saved game currently in use by the game.
 * @return Pointer to the saved game.
 */
SavedGame *Game::getSavedGame() const
{
	return _save;
}

/**
 * Sets a new saved game for the game to use.
 * @param save Pointer to the saved game.
 */
void Game::setSavedGame(SavedGame *save)
{
	delete _save;
	_save = save;
}

/**
 * Returns the mod currently in use by the game.
 * @return Pointer to the mod.
 */
Mod *Game::getMod() const
{
	return _mod;
}

/**
 * Loads the mods specified in the game options.
 */
void Game::loadMods()
{
	Mod::resetGlobalStatics();
	delete _mod;
	_mod = new Mod();
	_mod->loadAll(FileMap::getRulesets());
}

/**
 * Sets whether the mouse is activated.
 * If it is, mouse events are processed, otherwise
 * they are ignored and the cursor is hidden.
 * @param active Is mouse activated?
 */
void Game::setMouseActive(bool active)
{
	_mouseActive = active;
	_cursor->setVisible(active);
}

/**
 * Returns whether current state is *state
 * @param state The state to test against the stack state
 * @return Is state the current state?
 */
bool Game::isState(State *state) const
{
	return !_states.empty() && _states.back() == state;
}

/**
 * Checks if the game is currently quitting.
 * @return whether the game is shutting down or not.
 */
bool Game::isQuitting() const
{
	return _quit;
}

/**
 * Loads the most appropriate languages
 * given current system and game options.
 */
void Game::loadLanguages()
{
	const std::string defaultLang = "en-US";
	std::string currentLang = defaultLang;

	std::ostringstream ss;
	ss << "common/Language/" << defaultLang << ".yml";
	std::string defaultPath = CrossPlatform::searchDataFile(ss.str());
	std::string path = defaultPath;

	// No language set, detect based on system
	if (Options::language.empty())
	{
		std::string locale = CrossPlatform::getLocale();
		std::string lang = locale.substr(0, locale.find_first_of('-'));
		// Try to load full locale
		Unicode::replace(path, defaultLang, locale);
		if (Language::isSupported(locale) && CrossPlatform::fileExists(path))
		{
			currentLang = locale;
		}
		else
		{
			// Try to load language locale
			Unicode::replace(path, locale, lang);
			if (Language::isSupported(lang) && CrossPlatform::fileExists(path))
			{
				currentLang = lang;
			}
			// Give up, use default
			else
			{
				currentLang = defaultLang;
			}
		}
	}
	else
	{
		// Use options language
		Unicode::replace(path, defaultLang, Options::language);
		if (CrossPlatform::fileExists(path))
		{
			currentLang = Options::language;
		}
		// Language not found, use default
		else
		{
			currentLang = defaultLang;
		}
	}
	Options::language = currentLang;

	delete _lang;
	_lang = new Language();

	// Load default and current language
	std::ostringstream ssDefault, ssCurrent;
	ssDefault << "/Language/" << defaultLang << ".yml";
	ssCurrent << "/Language/" << currentLang << ".yml";

	_lang->loadFile(CrossPlatform::searchDataFile("common" + ssDefault.str()));
	if (currentLang != defaultLang)
		_lang->loadFile(CrossPlatform::searchDataFile("common" + ssCurrent.str()));

	// if this is a master but it has a master of its own, allow it to
	// chainload the "super" master, including its languages
	ModInfo modInfo = Options::getModInfo(Options::getActiveMaster());
	if (!modInfo.getMaster().empty())
	{
		ModInfo masterInfo = Options::getModInfo(modInfo.getMaster());
		_lang->loadFile(masterInfo.getPath() + ssDefault.str());
		if (currentLang != defaultLang)
			_lang->loadFile(masterInfo.getPath() + ssCurrent.str());
	}

	std::vector<const ModInfo*> activeMods = Options::getActiveMods();
	for (std::vector<const ModInfo*>::const_iterator i = activeMods.begin(); i != activeMods.end(); ++i)
	{
		_lang->loadFile((*i)->getPath() + ssDefault.str());
		if (currentLang != defaultLang)
			_lang->loadFile((*i)->getPath() + ssCurrent.str());
	}

	_lang->loadRule(_mod->getExtraStrings(), defaultLang);
	if (currentLang != defaultLang)
		_lang->loadRule(_mod->getExtraStrings(), currentLang);
}

/**
 * Initializes the audio subsystem.
 */
void Game::initAudio()
{
	Uint16 format = MIX_DEFAULT_FORMAT;
	if (Options::audioBitDepth == 8)
		format = AUDIO_S8;

	if (Options::audioSampleRate % 11025 != 0)
	{
		Log(LOG_WARNING) << "Custom sample rate " << Options::audioSampleRate << "Hz, audio that doesn't match will be distorted!";
		Log(LOG_WARNING) << "SDL_mixer only supports multiples of 11025Hz.";
	}
	int minChunk = Options::audioSampleRate / 11025 * 512;
	Options::audioChunkSize = std::max(minChunk, Options::audioChunkSize);

	if (Mix_OpenAudio(Options::audioSampleRate, format, MIX_DEFAULT_CHANNELS, Options::audioChunkSize) != 0)
	{
		Log(LOG_ERROR) << Mix_GetError();
		Log(LOG_WARNING) << "No sound device detected, audio disabled.";
		Options::mute = true;
	}
	else
	{
		Mix_AllocateChannels(16);
		// Set up UI channels
		Mix_ReserveChannels(4);
		Mix_GroupChannels(1, 2, 0);
		Log(LOG_INFO) << "SDL_mixer initialized successfully.";
		setVolume(Options::soundVolume, Options::musicVolume, Options::uiVolume);
	}
}

}
