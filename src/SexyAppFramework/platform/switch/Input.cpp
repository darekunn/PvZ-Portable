/*
 * Portions of this file are based on the PopCap Games Framework
 * Copyright (C) 2005-2009 PopCap Games, Inc.
 * 
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later AND LicenseRef-PopCap
 *
 * This file is part of PvZ-Portable.
 *
 * PvZ-Portable is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PvZ-Portable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with PvZ-Portable. If not, see <https://www.gnu.org/licenses/>.
 */

#include <switch.h>
#include <unordered_map>
#include <algorithm>
#include <cmath>

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "graphics/Graphics.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

static PadState pad;

// Buttons that are forwarded as keyboard keys.
// A / B are NOT here: they act as left / right mouse click (see below).
static std::unordered_map<u64, KeyCode> keyMaps = {
	{HidNpadButton_Plus,  KEYCODE_ESCAPE},
	{HidNpadButton_Minus, KEYCODE_SPACE},
	{HidNpadButton_X,     KEYCODE_RETURN},
	{HidNpadButton_L,     KEYCODE_LBUTTON},
	{HidNpadButton_R,     KEYCODE_RBUTTON},
};

// ---------------------------------------------------------------------------
// Controller -> virtual mouse cursor
//
//   Left stick / D-pad : move the cursor
//   ZL (hold)          : slow, precise cursor movement
//   A                  : left click  (select, plant, collect sun, press buttons)
//   B                  : right click (cancel the plant / shovel you are holding)
//   X                  : Enter
//   + / -              : Escape (pause menu) / Space
//
// The cursor is kept in screen pixels (1280x720), the same space as touch
// coordinates, and converted with WidgetManager::RemapMouse like touch input.
// ---------------------------------------------------------------------------
static constexpr float kScreenW = 1280.0f;
static constexpr float kScreenH = 720.0f;
static constexpr float kStickDeadzone = 0.20f;   // fraction of full stick travel
static constexpr float kStickMaxSpeed = 900.0f;  // screen pixels per second
static constexpr float kDpadSpeed     = 450.0f;  // screen pixels per second
static constexpr float kSlowFactor    = 0.35f;   // multiplier while ZL is held

static float sCursorX = kScreenW / 2;
static float sCursorY = kScreenH / 2;
static bool  sCursorVisible = false;  // shown once a controller is used, hidden again on touch
static u64   sLastCursorTick = 0;

// Draws a small arrow at the virtual cursor position (game coordinates).
// Called from SexyAppBase::DrawDirtyStuff just before the frame is presented.
void SwitchDrawVirtualCursor(Graphics& g, WidgetManager& theWidgetManager)
{
	if (!sCursorVisible)
		return;

	int x = (int)sCursorX;
	int y = (int)sCursorY;
	theWidgetManager.RemapMouse(x, y);

	// The game image is stretched ~1.6x horizontally and ~1.2x vertically, so the
	// arrow is made narrower than tall to look roughly right on screen.
	const int H = 24;

	g.SetColor(Color(0, 0, 0));
	for (int i = -1; i <= H; i++)
		g.FillRect(x - 1, y + i, (i * 11) / H + 3, 1);

	g.SetColor(Color(255, 255, 255));
	for (int i = 0; i < H; i++)
		g.FillRect(x, y + i, (i * 11) / H + 1, 1);
}

void SexyAppBase::InitInput()
{
	// Configure our supported input layout: a single player with standard controller styles
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);

	// Initialize the default gamepad (which reads handheld mode inputs as well as the first connected controller)
	padInitializeDefault(&pad);

	hidInitializeTouchScreen();

	if (!mMouseIn)
		mMouseIn = true;
}

bool SexyAppBase::StartTextInput(std::string& theInput)
{
	char buf[512] = {0};

	SwkbdConfig kbd;
	swkbdCreate(&kbd, 0);
	swkbdConfigMakePresetDefault(&kbd);
	swkbdConfigSetType(&kbd, SwkbdType_Normal);

	swkbdConfigSetGuideText(&kbd, "Enter text...");
	swkbdConfigSetOkButtonText(&kbd, "OK");

	Result rc = swkbdShow(&kbd, buf, sizeof(buf));
	swkbdClose(&kbd);

	if (R_SUCCEEDED(rc))
	{
		theInput = buf;
		return true;
	}

	return false;
}

void SexyAppBase::StopTextInput()
{
	
}

bool SexyAppBase::ProcessDeferredMessages(bool singleMessage)
{
	if (!appletMainLoop())
	{
		mShutdown = true;
		return false;
	}

	static s32 prev_touchcount = 0;

	// Scan the gamepad. This should be done once for each frame
	padUpdate(&pad);

	u64 kDown = padGetButtonsDown(&pad);
	u64 kUp = padGetButtonsUp(&pad);
	u64 kHeld = padGetButtons(&pad);

	if (kDown)
	{
		mLastUserInputTick = mLastTimerTime;

		for (auto& k : keyMaps)
		{
			if (kDown & k.first)
				mWidgetManager->KeyDown(k.second);
		}
	}

	if (kUp)
	{
		mLastUserInputTick = mLastTimerTime;

		for (auto& k : keyMaps)
		{
			if (kUp & k.first)
				mWidgetManager->KeyUp(k.second);
		}
	}

	// ---- Virtual cursor: left stick / D-pad move it, A / B click ----
	u64 aNow = armGetSystemTick();
	float aDt = 0.0f;
	if (sLastCursorTick != 0)
		aDt = (float)armTicksToNs(aNow - sLastCursorTick) / 1.0e9f;
	sLastCursorTick = aNow;
	aDt = std::min(aDt, 0.05f); // avoid a big jump after a long frame / pause

	float aVelX = 0.0f, aVelY = 0.0f; // screen pixels per second

	HidAnalogStickState aStick = padGetStickPos(&pad, 0);
	float aSx = aStick.x / 32767.0f;
	float aSy = -aStick.y / 32767.0f; // libnx reports +Y as up, the screen's +Y is down
	float aMag = std::sqrt(aSx * aSx + aSy * aSy);
	if (aMag > kStickDeadzone)
	{
		float aScaled = (std::min(aMag, 1.0f) - kStickDeadzone) / (1.0f - kStickDeadzone);
		float aSpeed = kStickMaxSpeed * aScaled * aScaled; // quadratic: fine control near the center
		aVelX = aSx / aMag * aSpeed;
		aVelY = aSy / aMag * aSpeed;
	}

	if (kHeld & HidNpadButton_Left)  aVelX -= kDpadSpeed;
	if (kHeld & HidNpadButton_Right) aVelX += kDpadSpeed;
	if (kHeld & HidNpadButton_Up)    aVelY -= kDpadSpeed;
	if (kHeld & HidNpadButton_Down)  aVelY += kDpadSpeed;

	if (kHeld & HidNpadButton_ZL)
	{
		aVelX *= kSlowFactor;
		aVelY *= kSlowFactor;
	}

	const u64 kClickButtons = HidNpadButton_A | HidNpadButton_B;
	bool aMoved = (aVelX != 0.0f || aVelY != 0.0f);
	bool aClickEvent = ((kDown | kUp) & kClickButtons) != 0;
	bool aWasVisible = sCursorVisible;

	if (aMoved)
	{
		sCursorX = std::clamp(sCursorX + aVelX * aDt, 0.0f, kScreenW - 1.0f);
		sCursorY = std::clamp(sCursorY + aVelY * aDt, 0.0f, kScreenH - 1.0f);
	}

	if (aMoved || aClickEvent)
		sCursorVisible = true;

	if (aMoved || aClickEvent)
	{
		int aGx = (int)sCursorX;
		int aGy = (int)sCursorY;
		mWidgetManager->RemapMouse(aGx, aGy);

		mLastUserInputTick = mLastTimerTime;
		mWidgetManager->MouseMove(aGx, aGy);

		if (kDown & HidNpadButton_A) mWidgetManager->MouseDown(aGx, aGy, 1);
		if (kUp   & HidNpadButton_A) mWidgetManager->MouseUp(aGx, aGy, 1);
		if (kDown & HidNpadButton_B) mWidgetManager->MouseDown(aGx, aGy, -1);
		if (kUp   & HidNpadButton_B) mWidgetManager->MouseUp(aGx, aGy, -1);
	}

	// The screen is only redrawn when something is dirty, so force a redraw
	// while the arrow moves, or when it appears.
	if (aMoved || sCursorVisible != aWasVisible)
		mWidgetManager->MarkAllDirty();

	// ---- Touch screen (handheld mode) ----
	HidTouchScreenState state = {0};
	hidGetTouchScreenStates(&state, 1);
	static int x=0, y=0;
	if (state.count)
	{
		mLastUserInputTick = mLastTimerTime;

		// Keep the virtual cursor under the finger and hide the arrow while touching
		sCursorX = (float)state.touches[0].x;
		sCursorY = (float)state.touches[0].y;
		if (sCursorVisible)
		{
			sCursorVisible = false;
			mWidgetManager->MarkAllDirty();
		}

		x = (int)state.touches[0].x;
		y = (int)state.touches[0].y;
		mWidgetManager->RemapMouse(x, y);
		mWidgetManager->MouseMove(x, y);
	}

	if (state.count && !prev_touchcount)
		mWidgetManager->MouseDown(x, y, 1);
	else if (!state.count && prev_touchcount)
		mWidgetManager->MouseUp(x, y, 1);

	prev_touchcount = state.count;

	return false;
}
