/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_REMOTE_STATE_HPP
#define _FOX_REMOTE_STATE_HPP

class Fox_Remote_State
{
public:
	static bool CommandActive(bool legacy_command_active);
	static bool CanAcceptCommand(bool legacy_command_active);
	static bool ShouldForceRender();
	static bool ScreenStreamActive();
	static bool ServerRunning();
	static bool ControlEnabled();
	static int ActiveJob();
};

#endif // _FOX_REMOTE_STATE_HPP
