/*  ZeroPAD - author: zerofrog(@gmail.com)
 *  Copyright (C) 2006-2007
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "linux.h"

Display *GSdsp;
static pthread_spinlock_t s_mutexStatus;
static u32 s_keyPress[2], s_keyRelease[2]; // thread safe

static const char* s_pGuiKeyMap[] = 
{ 
	"L2", "R2", "L1", "R1",
	"Triangle", "Circle", "Cross", "Square",
	"Select", "L3", "R3", "Start",
	"Up", "Right", "Down", "Left",
	"Lx", "Rx", "Ly", "Ry",
	"L_Up", "L_Right", "L_Down", "L_Left",
	"R_Up", "R_Right", "R_Down", "R_Left"
};

string GetLabelFromButton(const char* buttonname)
{
	string label = "e";
	label += buttonname;
	return label;
}

s32  _PADopen(void *pDsp)
{
	GSdsp = *(Display**)pDsp;
	pthread_spin_init(&s_mutexStatus, PTHREAD_PROCESS_PRIVATE);
	s_keyPress[0] = s_keyPress[1] = 0;
	s_keyRelease[0] = s_keyRelease[1] = 0;
	XAutoRepeatOff(GSdsp);

	JoystickInfo::EnumerateJoysticks(s_vjoysticks);

	return 0;
}

void _PADclose()
{
	pthread_spin_destroy(&s_mutexStatus);
	XAutoRepeatOn(GSdsp);

	vector<JoystickInfo*>::iterator it = s_vjoysticks.begin();
	
	// Delete everything in the vector vjoysticks.
	while (it != s_vjoysticks.end())
	{
		delete *it;
		it ++;
	}
	
	s_vjoysticks.clear();
}

void _PADupdate(int pad)
{
	pthread_spin_lock(&s_mutexStatus);
	status[pad] |= s_keyRelease[pad];
	status[pad] &= ~s_keyPress[pad];
	s_keyRelease[pad] = 0;
	s_keyPress[pad] = 0;
	pthread_spin_unlock(&s_mutexStatus);
}

int _GetJoystickIdFromPAD(int pad)
{
	// select the right joystick id
	int joyid = -1;
	
	for (int p = 0; p < PADSUBKEYS; p++)
	{
		for (int i = 0; i < PADKEYS; ++i)
		{
			u32 temp = conf.keys[(PadEnum[pad][p])][i];
			
			if (IS_JOYSTICK( temp) || IS_JOYBUTTONS(temp))
			{
				joyid = PAD_GETJOYID(temp);
				return joyid;
			}
		}
	}
	
	if ((joyid < 0) || (joyid >= (int)s_vjoysticks.size()))
	{
		// get first unused joystick
		for (joyid = 0; joyid < s_vjoysticks.size(); ++joyid)
		{
			if (s_vjoysticks[joyid]->GetPAD() < 0) break;
		}
	}
	
	return joyid;
}

void CALLBACK PADupdate(int pad)
{
	int i;
	XEvent E;
	int keyPress = 0, keyRelease = 0;
	KeySym key;

	// keyboard input
	while (XPending(GSdsp) > 0)
	{
		XNextEvent(GSdsp, &E);
		switch (E.type)
		{
			case KeyPress:
				key = XLookupKeysym((XKeyEvent *) & E, 0);

				i = FindKey(key, pad);
			
				// Analog controls.
				if ((i > PAD_RY) && (i <= PAD_R_LEFT))
				{
				switch (i)
				{
					case PAD_R_LEFT:
					case PAD_R_UP:
					case PAD_L_LEFT:
					case PAD_L_UP:
						Analog::ConfigurePad(Analog::AnalogToPad(i), pad, DEF_VALUE);
						break;
					case PAD_R_RIGHT:
					case PAD_R_DOWN:
					case PAD_L_RIGHT:
					case PAD_L_DOWN:
						Analog::ConfigurePad(Analog::AnalogToPad(i), pad, -DEF_VALUE);
						break;
				}
				i += 0xff00;
				}
				
				if (i != -1) 
				{
					clear_bit(keyRelease, i); 
					set_bit(keyPress, i);
				}
				//PAD_LOG("Key pressed:%d\n", i);

				event.evt = KEYPRESS;
				event.key = key;
				break;
				
			case KeyRelease:
				key = XLookupKeysym((XKeyEvent *) & E, 0);

				i = FindKey(key, pad);
			
				// Analog Controls.
				if ((i > PAD_RY) && (i <= PAD_R_LEFT))
				{
					Analog::ResetPad(Analog::AnalogToPad(i), pad);
					i += 0xff00;
				}
				
				if (i != -1) 
				{
					clear_bit(keyPress, i); 
					set_bit(keyRelease, i);
				}

				event.evt = KEYRELEASE;
				event.key = key;
				break;

			case FocusIn:
				XAutoRepeatOff(GSdsp);
				break;

			case FocusOut:
				XAutoRepeatOn(GSdsp);
				break;
		}
	}

	// joystick info
	SDL_JoystickUpdate();
	
	for (int i = 0; i < PADKEYS; i++)
	{
		int key = conf.keys[PadEnum[pad][0]][i];
		JoystickInfo* pjoy = NULL;

		if (IS_JOYBUTTONS(key))
		{
			int joyid = PAD_GETJOYID(key);
			
			if ((joyid >= 0) && (joyid < (int)s_vjoysticks.size()))
			{
				pjoy = s_vjoysticks[joyid];
				int pad = (pjoy)->GetPAD();
				int value = SDL_JoystickGetButton((pjoy)->GetJoy(), PAD_GETJOYBUTTON(key));
				
				if (value)
					clear_bit(status[pad], i); // pressed
				else
					set_bit(status[pad], i); // pressed
			}
		}
		else if (IS_JOYSTICK(key))
		{
			int joyid = PAD_GETJOYID(key);
			
			if ((joyid >= 0) && (joyid < (int)s_vjoysticks.size()))
			{
				pjoy = s_vjoysticks[joyid];
				int pad = (pjoy)->GetPAD();
				int value = SDL_JoystickGetAxis((pjoy)->GetJoy(), PAD_GETJOYSTICK_AXIS(key));
				
				switch (i)
				{
					case PAD_LX:
					case PAD_LY:
					case PAD_RX:
					case PAD_RY:
						if (abs(value) > (pjoy)->GetDeadzone(value))
							Analog::ConfigurePad(i, pad, value);
						else 
							Analog::ResetPad(i, pad);
						break;
				}
			}
		}
#ifdef EXPERIMENTAL_POV_CODE
		else if (IS_HAT(key))
		{
			int joyid = PAD_GETJOYID(key);
			
			if ((joyid >= 0) && (joyid < (int)s_vjoysticks.size()))
			{
				pjoy = s_vjoysticks[joyid];
				int pad = (pjoy)->GetPAD();
				int value = SDL_JoystickGetHat((pjoy)->GetJoy(), PAD_GETJOYSTICK_AXIS(key));
				
				//PAD_LOG("Hat = %d for key %d\n", PAD_GETPOVDIR(key), key);
				if ((value != SDL_HAT_CENTERED) && (PAD_GETHATDIR(key) == value))
				{
					if ((value == SDL_HAT_UP) || (value == SDL_HAT_RIGHT) || (value == SDL_HAT_DOWN) ||(value == SDL_HAT_LEFT))
					{
						set_bit(status[pad], i);
						PAD_LOG("Registered %s. Set (%d)\n", HatName(value), i);
					}
					else
					{
						clear_bit(status[pad], i);
					}
				}
				else
				{
					clear_bit(status[pad], i);
				}
			}
		}
#endif
		else if (IS_POV(key))
		{
			int joyid = PAD_GETJOYID(key);
			
			if (joyid >= 0 && (joyid < (int)s_vjoysticks.size()))
			{
				pjoy = s_vjoysticks[joyid];
				int pad = (pjoy)->GetPAD();
				int value = SDL_JoystickGetAxis((pjoy)->GetJoy(), PAD_GETJOYSTICK_AXIS(key));
				
				if (PAD_GETPOVSIGN(key) && (value < -2048))
					clear_bit(status[pad], i);
				else if (!PAD_GETPOVSIGN(key) && (value > 2048))
					clear_bit(status[pad], i);
				else
					set_bit(status[pad], i);
				
			
			}
		}
	}

	pthread_spin_lock(&s_mutexStatus);
	s_keyPress[pad] |= keyPress;
	s_keyPress[pad] &= ~keyRelease; 
	s_keyRelease[pad] |= keyRelease;
	s_keyRelease[pad] &= ~keyPress;
	pthread_spin_unlock(&s_mutexStatus);
}

void UpdateConf(int pad)
{
	initLogging();
	s_selectedpad = pad;

	int i;
	GtkWidget *Btn;
	for (i = 0; i < ArraySize(s_pGuiKeyMap); i++)
	{
		string tmp;

		if (s_pGuiKeyMap[i] == NULL) continue;

		Btn = lookup_widget(Conf, GetLabelFromButton(s_pGuiKeyMap[i]).c_str());
		if (Btn == NULL)
		{
			PAD_LOG("ZeroPAD: cannot find key %s\n", s_pGuiKeyMap[i]);
			continue;
		}
		
		if (IS_KEYBOARD(conf.keys[pad][i]))
		{
			char* pstr = XKeysymToString(PAD_GETKEY(conf.keys[pad][i]));
			if (pstr != NULL) tmp = pstr;
		}
		else if (IS_JOYBUTTONS(conf.keys[pad][i]))
		{
			int button = PAD_GETJOYBUTTON(conf.keys[pad][i]);
			tmp.resize(28);
			
			sprintf(&tmp[0], "JBut %d", button);
		}
		else if (IS_JOYSTICK(conf.keys[pad][i]))
		{
			int axis = PAD_GETJOYSTICK_AXIS(conf.keys[pad][i]);
			tmp.resize(28);
			
			sprintf(&tmp[0], "JAxis %d", axis);
		}
#ifdef EXPERIMENTAL_POV_CODE
		else if (IS_HAT(conf.keys[pad][i]))
		{
			int axis = PAD_GETJOYSTICK_AXIS(conf.keys[pad][i]);
			tmp.resize(28);
			
			switch(PAD_GETHATDIR(conf.keys[pad][i]))
			{
				case SDL_HAT_UP:
					sprintf(&tmp[0], "JPOVU-%d", axis);
					break;
					
				case SDL_HAT_RIGHT:
					sprintf(&tmp[0], "JPOVR-%d", axis);
					break;
					
				case SDL_HAT_DOWN:
					sprintf(&tmp[0], "JPOVD-%d", axis);
					break;
					
				case SDL_HAT_LEFT:
					sprintf(&tmp[0], "JPOVL-%d", axis);
					break;
			}
		}
#endif
		else if (IS_POV(conf.keys[pad][i]))
		{
			tmp.resize(28);
			sprintf(&tmp[0], "JPOV %d%s", PAD_GETJOYSTICK_AXIS(conf.keys[pad][i]), PAD_GETPOVSIGN(conf.keys[pad][i]) ? "-" : "+");
		}

		if (tmp.size() > 0)
		{
			gtk_entry_set_text(GTK_ENTRY(Btn), tmp.c_str());
		}
		else
			gtk_entry_set_text(GTK_ENTRY(Btn), "Unknown");

		gtk_object_set_user_data(GTK_OBJECT(Btn), (void*)(PADKEYS * pad + i));
	}

	// check bounds
	int joyid = _GetJoystickIdFromPAD(pad);

	if ((joyid >= 0) && (joyid < (int)s_vjoysticks.size()))
		gtk_combo_box_set_active(GTK_COMBO_BOX(s_devicecombo), joyid); // select the combo
	else 
		gtk_combo_box_set_active(GTK_COMBO_BOX(s_devicecombo), s_vjoysticks.size()); // no gamepad

	int padopts = conf.options >> (16 * pad);
	
	set_checked(Conf, "checkbutton_reverselx", padopts & PADOPTION_REVERTLX);
	set_checked(Conf, "checkbutton_reversely", padopts & PADOPTION_REVERTLY);
	set_checked(Conf, "checkbutton_reverserx", padopts & PADOPTION_REVERTRX);
	set_checked(Conf, "checkbutton_reversery", padopts & PADOPTION_REVERTRY);
	set_checked(Conf, "forcefeedback", padopts & PADOPTION_FORCEFEEDBACK);
}

GtkWidget* GetLabelWidget(GtkButton *button)
{
	const char* buttonname = gtk_widget_get_name(GTK_WIDGET(button));
	const char* labelname = GetLabelFromButton(buttonname).c_str();
	return lookup_widget(Conf, labelname);
}

int GetLabelId(GtkWidget *label)
{
	if (label == NULL)
	{
		PAD_LOG("couldn't find correct label\n");
		return -1;
	}

	return (int)(uptr)gtk_object_get_user_data(GTK_OBJECT(label));	
}

bool PollKeyboard(char* &temp, u32 &pkey)
{
	GdkEvent *ev = gdk_event_get();
	
	if (ev != NULL)
	{
		if (ev->type == GDK_KEY_PRESS)
		{
			
			if (ev->key.keyval == GDK_Escape) 
			{
				temp = "Unknown";
				pkey = NULL;
			}
			else
			{
				temp = XKeysymToString(ev->key.keyval);
				pkey = ev->key.keyval;
			}
			
			return true;
		}
	}
	
	return false;
}

void OnConf_Key(GtkButton *button, gpointer user_data)
{
	GtkWidget* label = GetLabelWidget(button);
	bool captured = false;
	char str[32];
	int id = GetLabelId(label);
	
	if (id == -1) return;
	
	int pad = id / PADKEYS;
	int key = id % PADKEYS;

	// save the joystick states
	UpdateJoysticks();
	
	while (!captured)
	{
		vector<JoystickInfo*>::iterator itjoy;
		char *tmp;
		
		if (PollKeyboard(tmp, conf.keys[pad][key])) 
		{
			strcpy(str, tmp);
			captured = true;
			break;
		}
		
		SDL_JoystickUpdate();
		
		itjoy = s_vjoysticks.begin();
		while ((itjoy != s_vjoysticks.end()) && (!captured))
		{
			int jbutton, direction;
			
			if ((*itjoy)->PollButtons(jbutton, conf.keys[pad][key]))
			{
				
				sprintf(str, "JBut %d", jbutton);
				captured = true;
				break;
			}

			bool negative = false;
			bool pov = (!((key == PAD_RY) || (key == PAD_LY) || (key == PAD_RX) || (key == PAD_LX)));
			
			if ((*itjoy)->PollAxes(pov, jbutton, negative, conf.keys[pad][key]))
			{
				if (pov)
					sprintf(str, "JPOV %d%s", jbutton, (negative) ? "-" : "+");
				else
					sprintf(str, "JAxis %d", jbutton);
				captured = true;
				break;
			}
			
#ifdef EXPERIMENTAL_POV_CODE
			if ((*itjoy)->PollHats(jbutton, direction, conf.keys[pad][key]))
			{
				switch (direction)
				{
					case SDL_HAT_UP: sprintf(str, "JPOVU-%d", jbutton); break;
					case SDL_HAT_RIGHT: sprintf(str, "JPOVR-%d", jbutton); break;
					case SDL_HAT_DOWN: sprintf(str, "JPOVD-%d", jbutton); break;
					case SDL_HAT_LEFT: sprintf(str, "JPOVL-%d", jbutton); break;	
				}
				captured = true;
				break;
			}
#endif
			itjoy++;
		}
	}
	
	gtk_entry_set_text(GTK_ENTRY(label), str);
}

void CALLBACK PADconfigure()
{
	char strcurdir[256];
	getcwd(strcurdir, 256);
	s_strIniPath = strcurdir;
	s_strIniPath += "/inis/zeropad.ini";

	LoadConfig();

	Conf = create_Conf();

	// recreate
	JoystickInfo::EnumerateJoysticks(s_vjoysticks);

	s_devicecombo = lookup_widget(Conf, "joydevicescombo");

	// fill the combo
	char str[255];
	vector<JoystickInfo*>::iterator it = s_vjoysticks.begin();
	
	// Delete everything in the vector vjoysticks.
	while (it != s_vjoysticks.end())
	{
		sprintf(str, "%d: %s - but: %d, axes: %d, hats: %d", (*it)->GetId(), (*it)->GetName().c_str(),
		        (*it)->GetNumButtons(), (*it)->GetNumAxes(), (*it)->GetNumHats());
		gtk_combo_box_append_text(GTK_COMBO_BOX(s_devicecombo), str);
		it++;
	}
	
	gtk_combo_box_append_text(GTK_COMBO_BOX(s_devicecombo), "No Gamepad");

	UpdateConf(0);

	gtk_widget_show_all(Conf);
	gtk_main();
}
