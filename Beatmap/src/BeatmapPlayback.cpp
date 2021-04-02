#include "stdafx.h"
#include "BeatmapPlayback.hpp"
#include "Shared/Profiling.hpp"

BeatmapPlayback::BeatmapPlayback(const Beatmap& beatmap) : m_beatmap(&beatmap)
{
}

bool BeatmapPlayback::Reset(MapTime initTime, MapTime start)
{
	m_effectObjects.clear();
	if (!m_beatmap || !m_beatmap->HasObjectState())
	{
		return false;
	}

	Logf("Resetting BeatmapPlayback, InitTime = %d, Start = %d", Logger::Severity::Debug, initTime, start);
	m_playbackTime = initTime;

	if (start <= 0) start = std::numeric_limits<decltype(start)>::min();
	m_viewRange = { start, start };

	m_currObject = m_beatmap->GetFirstObjectState();
	m_currLaserObject = m_beatmap->GetFirstObjectState();
	m_currAlertObject = m_beatmap->GetFirstObjectState();

	m_currentTiming = m_beatmap->GetFirstTimingPoint();
	m_currentLaneTogglePoint = m_beatmap->GetFirstLaneTogglePoint();

	m_currentTrackRollBehaviour = TrackRollBehaviour::Normal;
	m_lastTrackRollBehaviourChange = 0;

	//hittableLaserEnter = (*m_currentTiming)->beatDuration * 4.0;
	//alertLaserThreshold = (*m_currentTiming)->beatDuration * 6.0;

	m_hittableObjects.clear();
	m_holdObjects.clear();

	m_barTime = 0;
	m_beatTime = 0;
	m_initialEffectStateSent = false;

	return true;
}

void BeatmapPlayback::Update(MapTime newTime)
{
	MapTime delta = newTime - m_playbackTime;

	if (m_isCalibration) {
		// Count bars
		int32 beatID = 0;
		uint32 nBeats = CountBeats(m_playbackTime - delta, delta, beatID);
		const TimingPoint& tp = GetCurrentTimingPoint();
		double effectiveTime = ((double)newTime - tp.time); // Time with offset applied
		m_barTime = (float)fmod(effectiveTime / (tp.beatDuration * tp.numerator), 1.0);
		m_beatTime = (float)fmod(effectiveTime / tp.beatDuration, 1.0);

		// Set new time
		m_playbackTime = newTime;
		return;
	}

	if (newTime < m_playbackTime)
	{
		// Don't allow backtracking
		//Logf("New time was before last time %ull -> %ull", Logger::Warning, m_playbackTime, newTime);
		return;
	}

	// Fire initial effect changes (only once)
	if (!m_initialEffectStateSent)
	{
		const BeatmapSettings& settings = m_beatmap->GetMapSettings();
		OnEventChanged.Call(EventKey::LaserEffectMix, settings.laserEffectMix);
		OnEventChanged.Call(EventKey::LaserEffectType, settings.laserEffectType);
		OnEventChanged.Call(EventKey::SlamVolume, settings.slamVolume);
		m_initialEffectStateSent = true;
	}

	// Count bars
	int32 beatID = 0;
	uint32 nBeats = CountBeats(m_playbackTime - delta, delta, beatID);
	const TimingPoint& tp = GetCurrentTimingPoint();
	double effectiveTime = ((double)newTime - tp.time); // Time with offset applied
	m_barTime = (float)fmod(effectiveTime / (tp.beatDuration * tp.numerator), 1.0);
	m_beatTime = (float)fmod(effectiveTime / tp.beatDuration, 1.0);

	// Set new time
	m_playbackTime = newTime;

	// Advance timing
	Beatmap::TimingPointsIterator timingEnd = m_SelectTimingPoint(m_playbackTime);
	if (timingEnd != m_currentTiming)
	{
		m_currentTiming = timingEnd;
		/// TODO: Investigate why this causes score to be too high
		//hittableLaserEnter = (*m_currentTiming)->beatDuration * 4.0;
		//alertLaserThreshold = (*m_currentTiming)->beatDuration * 6.0;
		OnTimingPointChanged.Call(m_currentTiming);
	}

	// Advance lane toggle
	Beatmap::LaneTogglePointsIterator laneToggleEnd = m_SelectLaneTogglePoint(m_playbackTime);
	if (laneToggleEnd != m_currentLaneTogglePoint)
	{
		m_currentLaneTogglePoint = laneToggleEnd;
		OnLaneToggleChanged.Call(m_currentLaneTogglePoint);
	}

	// Advance objects
	Beatmap::ObjectsIterator objEnd = m_SelectHitObject(m_playbackTime + hittableObjectEnter);
	if (objEnd != m_currObject)
	{
		for (auto it = m_currObject; it < objEnd; it++)
		{
			MultiObjectState* obj = *(*it).get();
			if (obj->type == ObjectType::Laser) continue;

			if (!m_viewRange.Includes(obj->time)) continue;
			if (obj->type == ObjectType::Hold && !m_viewRange.Includes(obj->time + obj->hold.duration, true)) continue;

			if (obj->type == ObjectType::Hold || obj->type == ObjectType::Single)
			{
				m_holdObjects.Add(*obj);
			}

			m_hittableObjects.AddUnique((*it).get());
			OnObjectEntered.Call((*it).get());
		}
		m_currObject = objEnd;
	}

	// Advance lasers
	objEnd = m_SelectHitObject(m_playbackTime + hittableLaserEnter);
	if (objEnd != m_currLaserObject)
	{
		for (auto it = m_currLaserObject; it < objEnd; it++)
		{
			MultiObjectState* obj = *(*it).get();
			if (obj->type != ObjectType::Laser) continue;

			if (!m_viewRange.Includes(obj->time)) continue;
			if (!m_viewRange.Includes(obj->time + obj->laser.duration, true)) continue;

			m_holdObjects.Add(*obj);
			m_hittableObjects.AddUnique((*it).get());
			OnObjectEntered.Call((*it).get());
		}
		m_currLaserObject = objEnd;
	}

	// Check for lasers within the alert time
	objEnd = m_SelectHitObject(m_playbackTime + alertLaserThreshold);
	if (objEnd != m_currAlertObject)
	{
		for (auto it = m_currAlertObject; it < objEnd; it++)
		{
			MultiObjectState* obj = **it;
			if (!m_viewRange.Includes(obj->time)) continue;

			if (obj->type == ObjectType::Laser)
			{
				LaserObjectState* laser = (LaserObjectState*)obj;
				if (!laser->prev)
					OnLaserAlertEntered.Call(laser);
			}
		}
		m_currAlertObject = objEnd;
	}

	// Check passed hittable objects
	MapTime objectPassTime = m_playbackTime - hittableObjectLeave;
	for (auto it = m_hittableObjects.begin(); it != m_hittableObjects.end();)
	{
		MultiObjectState* obj = **it;
		if (obj->type == ObjectType::Hold)
		{
			MapTime endTime = obj->hold.duration + obj->time;
			if (endTime < objectPassTime)
			{
				OnObjectLeaved.Call(*it);
				it = m_hittableObjects.erase(it);
				continue;
			}
			if (obj->hold.effectType != EffectType::None && // Hold button with effect
				obj->time - 100 <= m_playbackTime + audioOffset && endTime - 100 > m_playbackTime + audioOffset) // Hold button in active range
			{
				if (!m_effectObjects.Contains(*obj))
				{
					OnFXBegin.Call((HoldObjectState*)*it);
					m_effectObjects.Add(*obj);
				}
			}
		}
		else if (obj->type == ObjectType::Laser)
		{
			if ((obj->laser.duration + obj->time) < objectPassTime)
			{
				OnObjectLeaved.Call(*it);
				it = m_hittableObjects.erase(it);
				continue;
			}
		}
		else if (obj->type == ObjectType::Single)
		{
			if (obj->time < objectPassTime)
			{
				OnObjectLeaved.Call(*it);
				it = m_hittableObjects.erase(it);
				continue;
			}
		}
		else if (obj->type == ObjectType::Event)
		{
			EventObjectState* evt = (EventObjectState*)obj;
			if (obj->time < (m_playbackTime + 2)) // Tiny offset to make sure events are triggered before they are needed
			{
				if (evt->key == EventKey::TrackRollBehaviour)
				{
					if (m_currentTrackRollBehaviour != evt->data.rollVal)
					{
						m_currentTrackRollBehaviour = evt->data.rollVal;
						m_lastTrackRollBehaviourChange = obj->time;
					}
				}

				// Trigger event
				OnEventChanged.Call(evt->key, evt->data);
				m_eventMapping[evt->key] = evt->data;
				it = m_hittableObjects.erase(it);
				continue;
			}
		}
		it++;
	}

	// Remove passed hold objects
	for (auto it = m_holdObjects.begin(); it != m_holdObjects.end();)
	{
		MultiObjectState* obj = **it;
		if (obj->type == ObjectType::Hold)
		{
			MapTime endTime = obj->hold.duration + obj->time;
			if (endTime < objectPassTime)
			{
				it = m_holdObjects.erase(it);
				continue;
			}
			if (endTime < m_playbackTime)
			{
				if (m_effectObjects.Contains(*it))
				{
					OnFXEnd.Call((HoldObjectState*)*it);
					m_effectObjects.erase(*it);
				}
			}
		}
		else if (obj->type == ObjectType::Laser)
		{
			if ((obj->laser.duration + obj->time) < objectPassTime)
			{
				it = m_holdObjects.erase(it);
				continue;
			}
		}
		else if (obj->type == ObjectType::Single)
		{
			if (obj->time < objectPassTime)
			{
				it = m_holdObjects.erase(it);
				continue;
			}
		}
		it++;
	}
}

void BeatmapPlayback::MakeCalibrationPlayback()
{
	m_isCalibration = true;

	for (size_t i = 0; i < 50; i++)
	{
		ButtonObjectState* newObject = new ButtonObjectState();
		newObject->index = i % 4;
		newObject->time = static_cast<MapTime>(i * 500);

		m_calibrationObjects.Add(Ref<ObjectState>((ObjectState*)newObject));
	}

	m_calibrationTiming = {};
	m_calibrationTiming.beatDuration = 500;
	m_calibrationTiming.time = 0;
	m_calibrationTiming.denominator = 4;
	m_calibrationTiming.numerator = 4;
}

Vector<ObjectState*> BeatmapPlayback::GetObjectsInRange(MapTime range)
{
	static const uint32 earlyVisibility = 200;

	const TimingPoint& tp = GetCurrentTimingPoint();

	MapTime begin = (MapTime) (m_playbackTime - earlyVisibility);
	MapTime end = m_playbackTime + range;

	Vector<ObjectState*> ret;

	if (m_isCalibration) {
		for (auto& o : m_calibrationObjects)
		{
			if (o->time < begin)
				continue;
			if (o->time > end)
				break;

			ret.Add(o.get());
		}
		return ret;
	}

	if (begin < m_viewRange.begin) begin = m_viewRange.begin;
	if (m_viewRange.HasEnd() && end >= m_viewRange.end) end = m_viewRange.end;

	// Add hold objects
	for (auto& ho : m_holdObjects)
	{
		ret.AddUnique(ho);
	}

	// Iterator
	Beatmap::ObjectsIterator obj = m_currObject;
	// Return all objects that lie after the currently queued object and fall within the given range
	while (!IsEndObject(obj))
	{
		if ((*obj)->time < begin)
		{
			obj += 1;
			continue;
		}

		if ((*obj)->time >= end)
			break; // No more objects

		ret.AddUnique((*obj).get());
		obj += 1; // Next
	}

	return ret;
}

const TimingPoint& BeatmapPlayback::GetCurrentTimingPoint() const
{
	if (m_isCalibration)
	{
		return m_calibrationTiming;
	}

	if (IsEndTiming(m_currentTiming))
	{
		return *(m_beatmap->GetFirstTimingPoint());
	}

	return *m_currentTiming;
}
const TimingPoint* BeatmapPlayback::GetTimingPointAt(MapTime time) const
{
	if (m_isCalibration)
	{
		return &m_calibrationTiming;
	}

	Beatmap::TimingPointsIterator it = const_cast<BeatmapPlayback*>(this)->m_SelectTimingPoint(time);
	if (IsEndTiming(it))
	{
		return nullptr;
	}
	else
	{
		return &(*it);
	}
}

uint32 BeatmapPlayback::CountBeats(MapTime start, MapTime range, int32& startIndex, uint32 multiplier /*= 1*/) const
{
	const TimingPoint& tp = GetCurrentTimingPoint();
	int64 delta = (int64)start - (int64)tp.time;
	double beatDuration = tp.GetWholeNoteLength() / tp.denominator;
	int64 beatStart = (int64)floor((double)delta / (beatDuration / multiplier));
	int64 beatEnd = (int64)floor((double)(delta + range) / (beatDuration / multiplier));
	startIndex = ((int32)beatStart + 1) % tp.numerator;
	return (uint32)Math::Max<int64>(beatEnd - beatStart, 0);
}

MapTime BeatmapPlayback::ViewDistanceToDuration(float distance)
{
	if (m_isCalibration)
	{
		return static_cast<MapTime>(distance * m_calibrationTiming.beatDuration);
	}

	Beatmap::TimingPointsIterator tp = m_SelectTimingPoint(m_playbackTime, true);

	double time = 0;

	MapTime currentTime = m_playbackTime;
	while (true)
	{
		if (!IsEndTiming(tp + 1))
		{
			double maxDist = ((tp + 1)->time - (double)currentTime) / tp->beatDuration;
			if (maxDist < distance)
			{
				// Split up
				time += maxDist * tp->beatDuration;
				distance -= (float)maxDist;
				tp++;
				continue;
			}
		}
		time += distance * tp->beatDuration;
		break;
	}

	// TODO: Optimize?

	/*
	uint32 processedStops = 0;
	Vector<ChartStop*> ignoreStops;
	do
	{
		processedStops = 0;
		for (auto cs : m_SelectChartStops(currentTime, time))
		{
			if (std::find(ignoreStops.begin(), ignoreStops.end(), cs) != ignoreStops.end())
				continue;
			time += cs->duration;
			processedStops++;
			ignoreStops.Add(cs);
		}
	} while (processedStops);
	*/

	return (MapTime)time;
}
float BeatmapPlayback::DurationToViewDistance(MapTime duration)
{
	return DurationToViewDistanceAtTime(m_playbackTime, duration);
}

float BeatmapPlayback::DurationToViewDistanceAtTimeNoStops(MapTime time, MapTime duration)
{
	MapTime endTime = time + duration;
	int8 direction = Math::Sign(duration);
	if (duration < 0)
	{
		MapTime temp = time;
		time = endTime;
		endTime = temp;
		duration *= -1;
	}

	// Accumulated value
	double barTime = 0.0f;

	// Split up to see if passing other timing points on the way
	Beatmap::TimingPointsIterator tp = m_SelectTimingPoint(time, true);
	while (true)
	{
		if (!IsEndTiming(tp + 1))
		{
			if ((tp+1)->time < endTime)
			{
				// Split up
				MapTime myDuration = (tp+1)->time - time;
				barTime += (double)myDuration / tp->beatDuration;
				duration -= myDuration;
				time = tp->time;
				tp++;
				continue;
			}
		}
		// Whole
		barTime += (double)duration / tp->beatDuration;
		break;
	}

	return (float)barTime * direction;
}

float BeatmapPlayback::DurationToViewDistanceAtTime(MapTime time, MapTime duration)
{
	if (cMod)
	{
		return (float)duration / 480000.0f;
	}

	MapTime endTime = time + duration;
	int8 direction = Math::Sign(duration);
	if (duration < 0)
	{
		MapTime temp = time;
		time = endTime;
		endTime = temp;
		duration *= -1;
	}

	if (m_isCalibration)
	{
		return duration / m_calibrationTiming.beatDuration * direction;
	}

	// Accumulated value
	double barTime = 0.0f;

	// Split up to see if passing other timing points on the way
	Beatmap::TimingPointsIterator tp = m_SelectTimingPoint(time, true);
	while (true)
	{
		if (!IsEndTiming(tp + 1))
		{
			if ((tp+1)->time < endTime)
			{
				// Split up
				MapTime myDuration = (tp+1)->time - time;
				barTime += (double)myDuration / tp->beatDuration;
				duration -= myDuration;
				time = (tp+1)->time;
				tp++;
				continue;
			}
		}
		// Whole
		barTime += (double)duration / tp->beatDuration;
		break;
	}

	MapTime startTime = time;

	// calculate stop ViewDistance
	/*
	double stopTime = 0.;
	for (auto cs : m_SelectChartStops(startTime, endTime - startTime))
	{
		MapTime overlap = Math::Min(abs(endTime - startTime), 
			Math::Min(abs(endTime - cs->time), 
				Math::Min(abs((cs->time + cs->duration) - startTime), abs((cs->time + cs->duration) - cs->time))));

		stopTime += DurationToViewDistanceAtTimeNoStops(Math::Max(cs->time, startTime), overlap);
	}
	barTime -= stopTime;
	*/

	return (float)barTime * direction;
}

float BeatmapPlayback::TimeToViewDistance(MapTime time)
{
	if (cMod)
		return (float)(time - m_playbackTime) / (480000.f);

	return DurationToViewDistanceAtTime(m_playbackTime, time - m_playbackTime);
}

float BeatmapPlayback::GetZoom(uint8 index)
{
	EffectTimeline::GraphType graphType;

	switch (index)
	{
	case 0:
		graphType = EffectTimeline::GraphType::ZOOM_BOTTOM;
		break;
	case 1:
		graphType = EffectTimeline::GraphType::ZOOM_TOP;
		break;
	case 2:
		graphType = EffectTimeline::GraphType::SHIFT_X;
		break;
	case 3:
		graphType = EffectTimeline::GraphType::ROTATION_Z;
		break;
	case 4:
		return m_beatmap->GetCenterSplitValueAt(m_playbackTime);
		break;
	default:
		assert(false);
		break;
	}

	// TODO: pass aux value
	return m_beatmap->GetGraphValueAt(graphType, m_playbackTime);
}

bool BeatmapPlayback::CheckIfManualTiltInstant()
{
	if (m_currentTrackRollBehaviour != TrackRollBehaviour::Manual)
	{
		return false;
	}

	// TODO: pass aux value
	return m_beatmap->CheckIfManualTiltInstant(m_lastTrackRollBehaviourChange, m_playbackTime);
}

Beatmap::TimingPointsIterator BeatmapPlayback::m_SelectTimingPoint(MapTime time, bool allowReset)
{
	Beatmap::TimingPointsIterator objStart = m_currentTiming;
	if (IsEndTiming(objStart))
		return objStart;

	// Start at front of array if current object lies ahead of given input time
	if (objStart->time > time && allowReset)
		objStart = m_beatmap->GetFirstTimingPoint();

	// Keep advancing the start pointer while the next object's starting time lies before the input time
	while (true)
	{
		if (!IsEndTiming(objStart + 1) && (objStart+1)->time <= time)
		{
			objStart = objStart + 1;
		}
		else
			break;
	}

	return objStart;
}

Beatmap::LaneTogglePointsIterator BeatmapPlayback::m_SelectLaneTogglePoint(MapTime time, bool allowReset)
{
	Beatmap::LaneTogglePointsIterator objStart = m_currentLaneTogglePoint;

	if (IsEndLaneToggle(objStart))
		return objStart;

	// Start at front of array if current object lies ahead of given input time
	if (objStart->time > time && allowReset)
		objStart = m_beatmap->GetFirstLaneTogglePoint();

	// Keep advancing the start pointer while the next object's starting time lies before the input time
	while (true)
	{
		if (!IsEndLaneToggle(objStart + 1) && (objStart + 1)->time <= time)
		{
			objStart = objStart + 1;
		}
		else
			break;
	}

	return objStart;
}

Beatmap::ObjectsIterator BeatmapPlayback::m_SelectHitObject(MapTime time, bool allowReset)
{
	Beatmap::ObjectsIterator objStart = m_currObject;
	if (IsEndObject(objStart))
		return objStart;

	// Start at front of array if current object lies ahead of given input time
	if (objStart[0]->time > time && allowReset)
		objStart = m_beatmap->GetFirstObjectState();

	// Keep advancing the start pointer while the next object's starting time lies before the input time
	while (true)
	{
		if (!IsEndObject(objStart) && objStart[0]->time < time)
		{
			objStart = std::next(objStart);
		}
		else
			break;
	}

	return objStart;
}

bool BeatmapPlayback::IsEndObject(const Beatmap::ObjectsIterator& obj) const
{
	return obj == m_beatmap->GetEndObjectState();
}

bool BeatmapPlayback::IsEndTiming(const Beatmap::TimingPointsIterator& obj) const
{
	return obj == m_beatmap->GetEndTimingPoint();
}

bool BeatmapPlayback::IsEndLaneToggle(const Beatmap::LaneTogglePointsIterator& obj) const
{
	return obj == m_beatmap->GetEndLaneTogglePoint();
}