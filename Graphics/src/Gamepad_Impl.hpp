#pragma once
#include "Gamepad.hpp"

#include "SDL2/SDL_joystick.h"

namespace Graphics
{
	class Window;
	
	/// Wrapper for SDL_JoystickGUID
	struct JoystickGUID
	{
		JoystickGUID(const SDL_JoystickGUID& value) : m_value(value) {}
		explicit JoystickGUID(const String& str) : m_value(SDL_JoystickGetGUIDFromString(str.data())) {}
		explicit JoystickGUID(SDL_Joystick* joystick) : m_value(SDL_JoystickGetGUID(joystick)) {}

		SDL_JoystickGUID m_value;

		bool operator< (const JoystickGUID& other) const
		{
			for (uint_fast8_t i = 0; i < 16; ++i) 
			{
				if (m_value.data[i] != other.m_value.data[i]) return m_value.data[i] < other.m_value.data[i];
			}

			return false;
		}

		String ToString() const
		{
			char joystickGUIDStr[35] = {'\0'};
			SDL_JoystickGetGUIDString(m_value, joystickGUIDStr, sizeof(joystickGUIDStr));

			return String(joystickGUIDStr);
		}
	};
	
	class Gamepad_Impl : public Gamepad
	{
	public:
		~Gamepad_Impl();
		bool Init(Graphics::Window* window, uint32 deviceIndex);

		// Handles input events straight from the event loop
		void HandleInputEvent(uint32 buttonIndex, uint8 newState);
		void HandleAxisEvent(uint32 axisIndex, int16 newValue);
		void HandleHatEvent(uint32 hadIndex, uint8 newValue);

		class Window* m_window;
		uint32 m_deviceIndex;
		SDL_Joystick* m_joystick;

		Vector<float> m_axisState;
		Vector<uint8> m_buttonStates;

		virtual bool GetButton(uint8 button) const override;
		virtual float GetAxis(uint8 idx) const override;
		virtual uint32 NumButtons() const override;
		virtual uint32 NumAxes() const override;
	};
}