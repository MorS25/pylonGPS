#include "event.hpp"

/**
Constructs the object without any submessages, set to occur/expire at the given time
@param inputTime: The time that the event times out or occurs
*/
event::event(const std::chrono::steady_clock::time_point &inputTime)
{
time = inputTime;
} 


/**
This function returns left.time > right.time
@param inputLeftEvent: The left side of >
@param inputRightEvent: The right side of >
@return: inputLeftEvent > inputRightEvent
*/
bool operator>(const event &inputLeftEvent, const event &inputRightEvent)
{
return inputLeftEvent.time > inputRightEvent.time;
}
