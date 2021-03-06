/*
 *      Copyright (C) 2012-2013 Team XBMC
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "PVRGUIInfo.h"

#include <cmath>
#include <ctime>

#include "Application.h"
#include "GUIInfoManager.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "guilib/GUIComponent.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/guiinfo/GUIInfo.h"
#include "guilib/guiinfo/GUIInfoHelper.h"
#include "guilib/guiinfo/GUIInfoLabels.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "utils/StringUtils.h"

#include "pvr/PVRGUIActions.h"
#include "pvr/PVRItem.h"
#include "pvr/PVRManager.h"
#include "pvr/addons/PVRClients.h"
#include "pvr/channels/PVRChannel.h"
#include "pvr/channels/PVRChannelGroupsContainer.h"
#include "pvr/channels/PVRRadioRDSInfoTag.h"
#include "pvr/epg/EpgInfoTag.h"
#include "pvr/recordings/PVRRecordings.h"
#include "pvr/timers/PVRTimers.h"

using namespace PVR;
using namespace KODI::GUILIB::GUIINFO;

CPVRGUIInfo::CPVRGUIInfo(void) :
    CThread("PVRGUIInfo")
{
  ResetProperties();
}

CPVRGUIInfo::~CPVRGUIInfo(void)
{
}

void CPVRGUIInfo::ResetProperties(void)
{
  CSingleLock lock(m_critSection);
  m_anyTimersInfo.ResetProperties();
  m_tvTimersInfo.ResetProperties();
  m_radioTimersInfo.ResetProperties();
  m_bHasTVRecordings            = false;
  m_bHasRadioRecordings         = false;
  m_iCurrentActiveClient        = 0;
  m_strPlayingClientName        .clear();
  m_strBackendName              .clear();
  m_strBackendVersion           .clear();
  m_strBackendHost              .clear();
  m_strBackendTimers            .clear();
  m_strBackendRecordings        .clear();
  m_strBackendDeletedRecordings .clear();
  m_strBackendChannels          .clear();
  m_iBackendDiskTotal           = 0;
  m_iBackendDiskUsed            = 0;
  m_iDuration                   = 0;
  m_bIsPlayingTV                = false;
  m_bIsPlayingRadio             = false;
  m_bIsPlayingRecording         = false;
  m_bIsPlayingEpgTag            = false;
  m_bIsPlayingEncryptedStream   = false;
  m_bIsRecordingPlayingChannel  = false;
  m_bCanRecordPlayingChannel    = false;
  m_bHasTVChannels              = false;
  m_bHasRadioChannels           = false;
  m_bHasTimeshiftData           = false;
  m_bIsTimeshifting             = false;
  m_iStartTime                  = time_t(0);
  m_iTimeshiftStartTime         = time_t(0);
  m_iTimeshiftEndTime           = time_t(0);
  m_iTimeshiftPlayTime          = time_t(0);
  m_iTimeshiftOffset            = 0;

  ResetPlayingTag();
  ClearQualityInfo(m_qualityInfo);
  ClearDescrambleInfo(m_descrambleInfo);

  m_updateBackendCacheRequested = false;
  m_bRegistered = false;
}

void CPVRGUIInfo::ClearQualityInfo(PVR_SIGNAL_STATUS &qualityInfo)
{
  memset(&qualityInfo, 0, sizeof(qualityInfo));
  strncpy(qualityInfo.strAdapterName, g_localizeStrings.Get(13106).c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
  strncpy(qualityInfo.strAdapterStatus, g_localizeStrings.Get(13106).c_str(), PVR_ADDON_NAME_STRING_LENGTH - 1);
}

void CPVRGUIInfo::ClearDescrambleInfo(PVR_DESCRAMBLE_INFO &descrambleInfo)
{
  descrambleInfo = {0};
}

void CPVRGUIInfo::Start(void)
{
  ResetProperties();
  Create();
  SetPriority(-1);
}

void CPVRGUIInfo::Stop(void)
{
  StopThread();
  CServiceBroker::GetPVRManager().UnregisterObserver(this);

  CGUIComponent* gui = CServiceBroker::GetGUI();
  if (gui)
  {
    gui->GetInfoManager().UnregisterInfoProvider(this);
    m_bRegistered = false;
  }
}

void CPVRGUIInfo::Notify(const Observable &obs, const ObservableMessage msg)
{
  if (msg == ObservableMessageTimers || msg == ObservableMessageTimersReset)
    UpdateTimersCache();
}

void CPVRGUIInfo::Process(void)
{
  unsigned int mLoop(0);
  int toggleInterval = g_advancedSettings.m_iPVRInfoToggleInterval / 1000;

  /* updated on request */
  CServiceBroker::GetPVRManager().RegisterObserver(this);
  UpdateTimersCache();

  /* update the backend cache once initially */
  m_updateBackendCacheRequested = true;

  while (!g_application.m_bStop && !m_bStop)
  {
    if (!m_bRegistered)
    {
      CGUIComponent* gui = CServiceBroker::GetGUI();
      if (gui)
      {
        gui->GetInfoManager().RegisterInfoProvider(this);
        m_bRegistered = true;
      }
    }

    if (!m_bStop)
      UpdateQualityData();
    Sleep(0);

    if (!m_bStop)
      UpdateDescrambleData();
    Sleep(0);

    if (!m_bStop)
      UpdateMisc();
    Sleep(0);

    if (!m_bStop)
      UpdateTimeshift();
    Sleep(0);

    if (!m_bStop)
      UpdatePlayingTag();
    Sleep(0);

    if (!m_bStop)
      UpdateTimersToggle();
    Sleep(0);

    if (!m_bStop)
      UpdateNextTimer();
    Sleep(0);

    // Update the backend cache every toggleInterval seconds
    if (!m_bStop && mLoop % toggleInterval == 0)
      UpdateBackendCache();

    if (++mLoop == 1000)
      mLoop = 0;

    if (!m_bStop)
      Sleep(1000);
  }

  if (!m_bStop)
    ResetPlayingTag();
}

void CPVRGUIInfo::UpdateQualityData(void)
{
  PVR_SIGNAL_STATUS qualityInfo;
  ClearQualityInfo(qualityInfo);

  if (CServiceBroker::GetSettings().GetBool(CSettings::SETTING_PVRPLAYBACK_SIGNALQUALITY))
  {
    bool bIsPlayingRecording = CServiceBroker::GetPVRManager().IsPlayingRecording();
    if (!bIsPlayingRecording)
    {
      CPVRClientPtr client;
      CServiceBroker::GetPVRManager().Clients()->GetCreatedClient(CServiceBroker::GetPVRManager().GetPlayingClientID(), client);
      if (client && client->SignalQuality(qualityInfo) == PVR_ERROR_NO_ERROR)
      {
        m_qualityInfo = qualityInfo;
      }
    }
  }
}

void CPVRGUIInfo::UpdateDescrambleData(void)
{
  PVR_DESCRAMBLE_INFO descrambleInfo;
  ClearDescrambleInfo(descrambleInfo);

  bool bIsPlayingRecording = CServiceBroker::GetPVRManager().IsPlayingRecording();
  if (!bIsPlayingRecording)
  {
    CPVRClientPtr client;
    CServiceBroker::GetPVRManager().Clients()->GetCreatedClient(CServiceBroker::GetPVRManager().GetPlayingClientID(), client);
    if (client && client->GetDescrambleInfo(descrambleInfo) == PVR_ERROR_NO_ERROR)
    {
      m_descrambleInfo = descrambleInfo;
    }
  }
}

void CPVRGUIInfo::UpdateMisc(void)
{
  bool bStarted = CServiceBroker::GetPVRManager().IsStarted();
  /* safe to fetch these unlocked, since they're updated from the same thread as this one */
  std::string strPlayingClientName     = bStarted ? CServiceBroker::GetPVRManager().GetPlayingClientName() : "";
  bool       bHasTVRecordings          = bStarted && CServiceBroker::GetPVRManager().Recordings()->GetNumTVRecordings() > 0;
  bool       bHasRadioRecordings       = bStarted && CServiceBroker::GetPVRManager().Recordings()->GetNumRadioRecordings() > 0;
  bool       bIsPlayingTV              = bStarted && CServiceBroker::GetPVRManager().IsPlayingTV();
  bool       bIsPlayingRadio           = bStarted && CServiceBroker::GetPVRManager().IsPlayingRadio();
  bool       bIsPlayingRecording       = bStarted && CServiceBroker::GetPVRManager().IsPlayingRecording();
  bool       bIsPlayingEpgTag          = bStarted && CServiceBroker::GetPVRManager().IsPlayingEpgTag();
  bool       bIsPlayingEncryptedStream = bStarted && CServiceBroker::GetPVRManager().IsPlayingEncryptedChannel();
  bool       bHasTVChannels            = bStarted && CServiceBroker::GetPVRManager().ChannelGroups()->GetGroupAllTV()->HasChannels();
  bool       bHasRadioChannels         = bStarted && CServiceBroker::GetPVRManager().ChannelGroups()->GetGroupAllRadio()->HasChannels();
  bool bCanRecordPlayingChannel        = bStarted && CServiceBroker::GetPVRManager().CanRecordOnPlayingChannel();
  bool bIsRecordingPlayingChannel      = bStarted && CServiceBroker::GetPVRManager().IsRecordingOnPlayingChannel();
  std::string strPlayingTVGroup        = (bStarted && bIsPlayingTV) ? CServiceBroker::GetPVRManager().GetPlayingGroup(false)->GroupName() : "";
  std::string strPlayingRadioGroup     = (bStarted && bIsPlayingRadio) ? CServiceBroker::GetPVRManager().GetPlayingGroup(true)->GroupName() : "";

  CSingleLock lock(m_critSection);
  m_strPlayingClientName      = strPlayingClientName;
  m_bHasTVRecordings          = bHasTVRecordings;
  m_bHasRadioRecordings       = bHasRadioRecordings;
  m_bIsPlayingTV              = bIsPlayingTV;
  m_bIsPlayingRadio           = bIsPlayingRadio;
  m_bIsPlayingRecording       = bIsPlayingRecording;
  m_bIsPlayingEpgTag          = bIsPlayingEpgTag;
  m_bIsPlayingEncryptedStream = bIsPlayingEncryptedStream;
  m_bHasTVChannels            = bHasTVChannels;
  m_bHasRadioChannels         = bHasRadioChannels;
  m_strPlayingTVGroup         = strPlayingTVGroup;
  m_strPlayingRadioGroup      = strPlayingRadioGroup;
  m_bCanRecordPlayingChannel  = bCanRecordPlayingChannel;
  m_bIsRecordingPlayingChannel = bIsRecordingPlayingChannel;
}

void CPVRGUIInfo::UpdateTimeshift(void)
{
  if (!CServiceBroker::GetPVRManager().IsPlayingTV() && !CServiceBroker::GetPVRManager().IsPlayingRadio())
  {
    // If nothing is playing (anymore), there is no need to poll the timeshift values from the clients.
    CSingleLock lock(m_critSection);
    if (m_bHasTimeshiftData)
    {
      m_bHasTimeshiftData = false;
      m_bIsTimeshifting = false;
      m_iStartTime = 0;
      m_iTimeshiftStartTime = 0;
      m_iTimeshiftEndTime = 0;
      m_iTimeshiftPlayTime = 0;
      m_iLastTimeshiftUpdate = 0;
      m_iTimeshiftOffset = 0;
    }
    return;
  }

  bool bIsTimeshifting = CServiceBroker::GetPVRManager().IsTimeshifting();
  time_t now = std::time(nullptr);
  time_t iStartTime = CServiceBroker::GetDataCacheCore().GetStartTime();
  time_t iPlayTime = CServiceBroker::GetDataCacheCore().GetPlayTime() / 1000;
  time_t iMinTime = bIsTimeshifting ? CServiceBroker::GetDataCacheCore().GetMinTime() / 1000 : 0;
  time_t iMaxTime = bIsTimeshifting ? CServiceBroker::GetDataCacheCore().GetMaxTime() / 1000 : 0;
  bool bPlaying = CServiceBroker::GetDataCacheCore().GetSpeed() == 1.0;

  CSingleLock lock(m_critSection);

  m_iLastTimeshiftUpdate = now;

  if (!iStartTime)
  {
    if (m_iStartTime == 0)
      iStartTime = now;
    else
      iStartTime = m_iStartTime;
  }

  m_bIsTimeshifting = bIsTimeshifting;
  m_iStartTime = iStartTime;
  m_iTimeshiftStartTime = iStartTime + iMinTime;
  m_iTimeshiftEndTime = iStartTime + iMaxTime;

  if (m_iTimeshiftEndTime > m_iTimeshiftStartTime)
  {
    // timeshifting supported
    m_iTimeshiftPlayTime = iStartTime + iPlayTime;
  }
  else if (bPlaying)
  {
    // timeshifting not supported
    m_iTimeshiftPlayTime = now - m_iTimeshiftOffset;
  }

  m_iTimeshiftOffset = now - m_iTimeshiftPlayTime;

  m_bHasTimeshiftData = true;
}

bool CPVRGUIInfo::InitCurrentItem(CFileItem *item)
{
  return false;
}

bool CPVRGUIInfo::GetLabel(std::string& value, const CFileItem *item, int contextWindow, const CGUIInfo &info, std::string *fallback) const
{
  return GetListItemAndPlayerLabel(item, info, value) ||
         GetPVRLabel(item, info, value) ||
         GetRadioRDSLabel(item, info, value);
}

bool CPVRGUIInfo::GetListItemAndPlayerLabel(const CFileItem *item, const CGUIInfo &info, std::string &strValue) const
{
  const CPVRTimerInfoTagPtr timer = item->GetPVRTimerInfoTag();
  if (timer)
  {
    switch (info.m_info)
    {
      case LISTITEM_DATE:
        strValue = timer->Summary();
        return true;
      case LISTITEM_STARTDATE:
        strValue = timer->StartAsLocalTime().GetAsLocalizedDate(true);
        return true;
      case LISTITEM_STARTTIME:
        strValue = timer->StartAsLocalTime().GetAsLocalizedTime("", false);
        return true;
      case LISTITEM_ENDDATE:
        strValue = timer->EndAsLocalTime().GetAsLocalizedDate(true);
        return true;
      case LISTITEM_ENDTIME:
        strValue = timer->EndAsLocalTime().GetAsLocalizedTime("", false);
        return true;
      case LISTITEM_TITLE:
        strValue = timer->Title();
        return true;
      case LISTITEM_COMMENT:
        strValue = timer->GetStatus();
        return true;
      case LISTITEM_TIMERTYPE:
        strValue = timer->GetTypeAsString();
        return true;
      case LISTITEM_CHANNEL_NAME:
        strValue = timer->ChannelName();
        return true;
      case LISTITEM_EPG_EVENT_TITLE:
      case LISTITEM_GENRE:
      case LISTITEM_PLOT:
      case LISTITEM_PLOT_OUTLINE:
      case LISTITEM_DURATION:
      case LISTITEM_ORIGINALTITLE:
      case LISTITEM_YEAR:
      case LISTITEM_SEASON:
      case LISTITEM_EPISODE:
      case LISTITEM_EPISODENAME:
      case LISTITEM_DIRECTOR:
      case LISTITEM_CHANNEL_NUMBER:
      case LISTITEM_PREMIERED:
        break; // obtain value from channel/epg
      default:
        return false;
    }
  }

  const CPVRRecordingPtr recording(item->GetPVRRecordingInfoTag());
  if (recording)
  {
    // Note: CPVRRecoding is derived from CVideoInfoTag. All base class properties will be handled
    //       by CGUIInfoManager. Only properties introduced by CPVRRecording need to be handled here.
    switch (info.m_info)
    {
      case LISTITEM_DATE:
        strValue = recording->RecordingTimeAsLocalTime().GetAsLocalizedDateTime(false, false);
        return true;
      case LISTITEM_STARTDATE:
        strValue = recording->RecordingTimeAsLocalTime().GetAsLocalizedDate(true);
        return true;
      case VIDEOPLAYER_STARTTIME:
      case LISTITEM_STARTTIME:
        strValue = recording->RecordingTimeAsLocalTime().GetAsLocalizedTime("", false);
        return true;
      case LISTITEM_ENDDATE:
        strValue = recording->EndTimeAsLocalTime().GetAsLocalizedDate(true);
        return true;
      case VIDEOPLAYER_ENDTIME:
      case LISTITEM_ENDTIME:
        strValue = recording->EndTimeAsLocalTime().GetAsLocalizedTime("", false);
        return true;
      case LISTITEM_EXPIRATION_DATE:
        if (recording->HasExpirationTime())
        {
          strValue = recording->ExpirationTimeAsLocalTime().GetAsLocalizedDate(false);
          return true;
        }
        break;
      case LISTITEM_EXPIRATION_TIME:
        if (recording->HasExpirationTime())
        {
          strValue = recording->ExpirationTimeAsLocalTime().GetAsLocalizedTime("", false);;
          return true;
        }
        break;
      case VIDEOPLAYER_EPISODENAME:
      case LISTITEM_EPISODENAME:
        strValue = recording->EpisodeName();
        return true;
      case VIDEOPLAYER_CHANNEL_NAME:
      case LISTITEM_CHANNEL_NAME:
        strValue = recording->m_strChannelName;
        return true;
      case VIDEOPLAYER_CHANNEL_NUMBER:
      case LISTITEM_CHANNEL_NUMBER:
      {
        const CPVRChannelPtr channel = recording->Channel();
        if (channel)
        {
          strValue = channel->ChannelNumber().FormattedChannelNumber();
          return true;
        }
        break;
      }
      case VIDEOPLAYER_CHANNEL_GROUP:
      {
        CSingleLock lock(m_critSection);
        strValue = recording->IsRadio() ? m_strPlayingRadioGroup : m_strPlayingTVGroup;
        return true;
      }
    }
    return false;
  }

  if (item->HasPVRRadioRDSInfoTag())
  {
    switch (info.m_info)
    {
      case PLAYER_TITLE:
        /* Load the RDS Radiotext+ if present */
        strValue = item->GetPVRRadioRDSInfoTag()->GetTitle();
        if (!strValue.empty())
          return true;
        /* If no plus present load the RDS Radiotext info line 0 if present */
        strValue = g_application.GetAppPlayer().GetRadioText(0);
        if (!strValue.empty())
          return true;
        break; // get title from epg
      case MUSICPLAYER_CHANNEL_NAME:
        strValue = item->GetPVRRadioRDSInfoTag()->GetProgStation();
        if (!strValue.empty())
          return true;
        break; // get channel name from channel tag
    }
  }

  CPVREpgInfoTagPtr epgTag;
  CPVRChannelPtr channel;
  if (item->IsPVRChannel() || item->IsEPG() || item->IsPVRTimer())
  {
    switch (info.m_info)
    {
      case VIDEOPLAYER_NEXT_TITLE:
      case VIDEOPLAYER_NEXT_GENRE:
      case VIDEOPLAYER_NEXT_PLOT:
      case VIDEOPLAYER_NEXT_PLOT_OUTLINE:
      case VIDEOPLAYER_NEXT_STARTTIME:
      case VIDEOPLAYER_NEXT_ENDTIME:
      case VIDEOPLAYER_NEXT_DURATION:
      case LISTITEM_NEXT_TITLE:
      case LISTITEM_NEXT_GENRE:
      case LISTITEM_NEXT_PLOT:
      case LISTITEM_NEXT_PLOT_OUTLINE:
      case LISTITEM_NEXT_STARTDATE:
      case LISTITEM_NEXT_STARTTIME:
      case LISTITEM_NEXT_ENDDATE:
      case LISTITEM_NEXT_ENDTIME:
      case LISTITEM_NEXT_DURATION:
      {
        CPVRItem pvrItem(item);
        epgTag = pvrItem.GetNextEpgInfoTag();
        channel = pvrItem.GetChannel();
        break;
      }
      default:
      {
        CPVRItem pvrItem(item);
        epgTag = pvrItem.GetEpgInfoTag();
        channel = pvrItem.GetChannel();
        break;
      }
    }

    switch (info.m_info)
    {
      // special handling for channels without epg or with radio rds data
      case PLAYER_TITLE:
      case VIDEOPLAYER_TITLE:
      case LISTITEM_TITLE:
      case VIDEOPLAYER_NEXT_TITLE:
      case LISTITEM_NEXT_TITLE:
      case LISTITEM_EPG_EVENT_TITLE:
        // Note: in difference to LISTITEM_TITLE, LISTITEM_EPG_EVENT_TITLE returns the title
        // associated with the epg event of a timer, if any, and not the title of the timer.
        if (epgTag)
          strValue = epgTag->Title();
        if (strValue.empty() && !CServiceBroker::GetSettings().GetBool(CSettings::SETTING_EPG_HIDENOINFOAVAILABLE))
          strValue = g_localizeStrings.Get(19055); // no information available
        return true;
    }
  }

  if (epgTag)
  {
    switch (info.m_info)
    {
      case VIDEOPLAYER_GENRE:
      case LISTITEM_GENRE:
      case VIDEOPLAYER_NEXT_GENRE:
      case LISTITEM_NEXT_GENRE:
        strValue = epgTag->GetGenresLabel();
        return true;
      case VIDEOPLAYER_PLOT:
      case LISTITEM_PLOT:
      case VIDEOPLAYER_NEXT_PLOT:
      case LISTITEM_NEXT_PLOT:
        strValue = epgTag->Plot();
        return true;
      case VIDEOPLAYER_PLOT_OUTLINE:
      case LISTITEM_PLOT_OUTLINE:
      case VIDEOPLAYER_NEXT_PLOT_OUTLINE:
      case LISTITEM_NEXT_PLOT_OUTLINE:
        strValue = epgTag->PlotOutline();
        return true;
      case LISTITEM_DATE:
        strValue = epgTag->StartAsLocalTime().GetAsLocalizedDateTime(false, false);
        return true;
      case LISTITEM_STARTDATE:
      case LISTITEM_NEXT_STARTDATE:
        strValue = epgTag->StartAsLocalTime().GetAsLocalizedDate(true);
        return true;
      case VIDEOPLAYER_STARTTIME:
      case VIDEOPLAYER_NEXT_STARTTIME:
      case LISTITEM_STARTTIME:
      case LISTITEM_NEXT_STARTTIME:
        strValue = epgTag->StartAsLocalTime().GetAsLocalizedTime("", false);
        return true;
      case LISTITEM_ENDDATE:
      case LISTITEM_NEXT_ENDDATE:
        strValue = epgTag->EndAsLocalTime().GetAsLocalizedDate(true);
        return true;
      case VIDEOPLAYER_ENDTIME:
      case VIDEOPLAYER_NEXT_ENDTIME:
      case LISTITEM_ENDTIME:
      case LISTITEM_NEXT_ENDTIME:
        strValue = epgTag->EndAsLocalTime().GetAsLocalizedTime("", false);
        return true;
      // note: for some reason, there is no VIDEOPLAYER_DURATION
      case LISTITEM_DURATION:
      case VIDEOPLAYER_NEXT_DURATION:
      case LISTITEM_NEXT_DURATION:
        if (epgTag->GetDuration() > 0)
        {
          strValue = StringUtils::SecondsToTimeString(epgTag->GetDuration(), static_cast<TIME_FORMAT>(info.GetData4()));
          return true;
        }
        return false;
      case VIDEOPLAYER_IMDBNUMBER:
      case LISTITEM_IMDBNUMBER:
        strValue = epgTag->IMDBNumber();
        return true;
      case VIDEOPLAYER_ORIGINALTITLE:
      case LISTITEM_ORIGINALTITLE:
        strValue = epgTag->OriginalTitle();
        return true;
      case VIDEOPLAYER_YEAR:
      case LISTITEM_YEAR:
        if (epgTag->Year() > 0)
        {
          strValue = StringUtils::Format("%i", epgTag->Year());
          return true;
        }
        return false;
      case VIDEOPLAYER_SEASON:
      case LISTITEM_SEASON:
        if (epgTag->SeriesNumber() > 0)
        {
          strValue = StringUtils::Format("%i", epgTag->SeriesNumber());
          return true;
        }
        return false;
      case VIDEOPLAYER_EPISODE:
      case LISTITEM_EPISODE:
        if (epgTag->EpisodeNumber() > 0)
        {
          if (epgTag->SeriesNumber() == 0) // prefix episode with 'S'
            strValue = StringUtils::Format("S%i", epgTag->EpisodeNumber());
          else
            strValue = StringUtils::Format("%i", epgTag->EpisodeNumber());
          return true;
        }
        return false;
      case VIDEOPLAYER_EPISODENAME:
      case LISTITEM_EPISODENAME:
        strValue = epgTag->EpisodeName();
        return true;
      case VIDEOPLAYER_CAST:
      case LISTITEM_CAST:
        strValue = epgTag->GetCastLabel();
        return true;
      case VIDEOPLAYER_DIRECTOR:
      case LISTITEM_DIRECTOR:
        strValue = epgTag->GetDirectorsLabel();
        return true;
      case VIDEOPLAYER_WRITER:
      case LISTITEM_WRITER:
        strValue = epgTag->GetWritersLabel();
        return true;
      case VIDEOPLAYER_PARENTAL_RATING:
      case LISTITEM_PARENTALRATING:
        if (epgTag->ParentalRating() > 0)
        {
          strValue = StringUtils::Format("%i", epgTag->ParentalRating());
          return true;
        }
        return false;
      case LISTITEM_PREMIERED:
        if (epgTag->FirstAiredAsLocalTime().IsValid())
        {
          strValue = epgTag->FirstAiredAsLocalTime().GetAsLocalizedDate(true);
          return true;
        }
        return false;
    }
  }

  if (channel)
  {
    switch (info.m_info)
    {
      case MUSICPLAYER_CHANNEL_NAME:
      case VIDEOPLAYER_CHANNEL_NAME:
      case LISTITEM_CHANNEL_NAME:
        strValue = channel->ChannelName();
        return true;
      case MUSICPLAYER_CHANNEL_NUMBER:
      case VIDEOPLAYER_CHANNEL_NUMBER:
      case LISTITEM_CHANNEL_NUMBER:
        strValue = channel->ChannelNumber().FormattedChannelNumber();
        return true;
      case MUSICPLAYER_CHANNEL_GROUP:
      case VIDEOPLAYER_CHANNEL_GROUP:
      {
        CSingleLock lock(m_critSection);
        strValue = channel->IsRadio() ? m_strPlayingRadioGroup : m_strPlayingTVGroup;
        return true;
      }
    }
  }

  return false;
}

bool CPVRGUIInfo::GetPVRLabel(const CFileItem *item, const CGUIInfo &info, std::string &strValue) const
{
  CSingleLock lock(m_critSection);

  switch (info.m_info)
  {
    case PVR_EPG_EVENT_DURATION:
      CharInfoEpgEventDuration(item, static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
   case PVR_EPG_EVENT_ELAPSED_TIME:
      CharInfoEpgEventElapsedTime(item, static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
    case PVR_EPG_EVENT_REMAINING_TIME:
      CharInfoEpgEventRemainingTime(item, static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
    case PVR_EPG_EVENT_FINISH_TIME:
      CharInfoEpgEventFinishTime(item, static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
    case PVR_TIMESHIFT_START_TIME:
      CharInfoTimeshiftStartTime(static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
    case PVR_TIMESHIFT_END_TIME:
      CharInfoTimeshiftEndTime(static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
    case PVR_TIMESHIFT_PLAY_TIME:
      CharInfoTimeshiftPlayTime(static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
    case PVR_TIMESHIFT_OFFSET:
      CharInfoTimeshiftOffset(static_cast<TIME_FORMAT>(info.GetData1()), strValue);
      return true;
    case PVR_EPG_EVENT_SEEK_TIME:
      strValue = StringUtils::SecondsToTimeString(GetElapsedTime() + g_application.GetAppPlayer().GetSeekHandler().GetSeekSize(),
                                                  static_cast<TIME_FORMAT>(info.GetData1())).c_str();
      return true;
    case PVR_NOW_RECORDING_TITLE:
      strValue = m_anyTimersInfo.GetActiveTimerTitle();
      return true;
    case PVR_NOW_RECORDING_CHANNEL:
      strValue = m_anyTimersInfo.GetActiveTimerChannelName();
      return true;
    case PVR_NOW_RECORDING_CHAN_ICO:
      strValue = m_anyTimersInfo.GetActiveTimerChannelIcon();
      return true;
    case PVR_NOW_RECORDING_DATETIME:
      strValue = m_anyTimersInfo.GetActiveTimerDateTime();
      return true;
    case PVR_NEXT_RECORDING_TITLE:
      strValue = m_anyTimersInfo.GetNextTimerTitle();
      return true;
    case PVR_NEXT_RECORDING_CHANNEL:
      strValue = m_anyTimersInfo.GetNextTimerChannelName();
      return true;
    case PVR_NEXT_RECORDING_CHAN_ICO:
      strValue = m_anyTimersInfo.GetNextTimerChannelIcon();
      return true;
    case PVR_NEXT_RECORDING_DATETIME:
      strValue = m_anyTimersInfo.GetNextTimerDateTime();
      return true;
    case PVR_TV_NOW_RECORDING_TITLE:
      strValue = m_tvTimersInfo.GetActiveTimerTitle();
      return true;
    case PVR_TV_NOW_RECORDING_CHANNEL:
      strValue = m_tvTimersInfo.GetActiveTimerChannelName();
      return true;
    case PVR_TV_NOW_RECORDING_CHAN_ICO:
      strValue = m_tvTimersInfo.GetActiveTimerChannelIcon();
      return true;
    case PVR_TV_NOW_RECORDING_DATETIME:
      strValue = m_tvTimersInfo.GetActiveTimerDateTime();
      return true;
    case PVR_TV_NEXT_RECORDING_TITLE:
      strValue = m_tvTimersInfo.GetNextTimerTitle();
      return true;
    case PVR_TV_NEXT_RECORDING_CHANNEL:
      strValue = m_tvTimersInfo.GetNextTimerChannelName();
      return true;
    case PVR_TV_NEXT_RECORDING_CHAN_ICO:
      strValue = m_tvTimersInfo.GetNextTimerChannelIcon();
      return true;
    case PVR_TV_NEXT_RECORDING_DATETIME:
      strValue = m_tvTimersInfo.GetNextTimerDateTime();
      return true;
    case PVR_RADIO_NOW_RECORDING_TITLE:
      strValue = m_radioTimersInfo.GetActiveTimerTitle();
      return true;
    case PVR_RADIO_NOW_RECORDING_CHANNEL:
      strValue = m_radioTimersInfo.GetActiveTimerChannelName();
      return true;
    case PVR_RADIO_NOW_RECORDING_CHAN_ICO:
      strValue = m_radioTimersInfo.GetActiveTimerChannelIcon();
      return true;
    case PVR_RADIO_NOW_RECORDING_DATETIME:
      strValue = m_radioTimersInfo.GetActiveTimerDateTime();
      return true;
    case PVR_RADIO_NEXT_RECORDING_TITLE:
      strValue = m_radioTimersInfo.GetNextTimerTitle();
      return true;
    case PVR_RADIO_NEXT_RECORDING_CHANNEL:
      strValue = m_radioTimersInfo.GetNextTimerChannelName();
      return true;
    case PVR_RADIO_NEXT_RECORDING_CHAN_ICO:
      strValue = m_radioTimersInfo.GetNextTimerChannelIcon();
      return true;
    case PVR_RADIO_NEXT_RECORDING_DATETIME:
      strValue = m_radioTimersInfo.GetNextTimerDateTime();
      return true;
    case PVR_NEXT_TIMER:
      strValue = m_anyTimersInfo.GetNextTimer();
      return true;
    case PVR_ACTUAL_STREAM_SIG:
      CharInfoSignal(strValue);
      return true;
    case PVR_ACTUAL_STREAM_SNR:
      CharInfoSNR(strValue);
      return true;
    case PVR_ACTUAL_STREAM_BER:
      CharInfoBER(strValue);
      return true;
    case PVR_ACTUAL_STREAM_UNC:
      CharInfoUNC(strValue);
      return true;
    case PVR_ACTUAL_STREAM_CLIENT:
      CharInfoPlayingClientName(strValue);
      return true;
    case PVR_ACTUAL_STREAM_DEVICE:
      CharInfoFrontendName(strValue);
      return true;
    case PVR_ACTUAL_STREAM_STATUS:
      CharInfoFrontendStatus(strValue);
      return true;
    case PVR_ACTUAL_STREAM_CRYPTION:
      CharInfoEncryption(strValue);
      return true;
    case PVR_ACTUAL_STREAM_SERVICE:
      CharInfoService(strValue);
      return true;
    case PVR_ACTUAL_STREAM_MUX:
      CharInfoMux(strValue);
      return true;
    case PVR_ACTUAL_STREAM_PROVIDER:
      CharInfoProvider(strValue);
      return true;
    case PVR_BACKEND_NAME:
      CharInfoBackendName(strValue);
      return true;
    case PVR_BACKEND_VERSION:
      CharInfoBackendVersion(strValue);
      return true;
    case PVR_BACKEND_HOST:
      CharInfoBackendHost(strValue);
      return true;
    case PVR_BACKEND_DISKSPACE:
      CharInfoBackendDiskspace(strValue);
      return true;
    case PVR_BACKEND_CHANNELS:
      CharInfoBackendChannels(strValue);
      return true;
    case PVR_BACKEND_TIMERS:
      CharInfoBackendTimers(strValue);
      return true;
    case PVR_BACKEND_RECORDINGS:
      CharInfoBackendRecordings(strValue);
      return true;
    case PVR_BACKEND_DELETED_RECORDINGS:
      CharInfoBackendDeletedRecordings(strValue);
      return true;
    case PVR_BACKEND_NUMBER:
      CharInfoBackendNumber(strValue);
      return true;
    case PVR_TOTAL_DISKSPACE:
      CharInfoTotalDiskSpace(strValue);
      return true;
    case PVR_CHANNEL_NUMBER_INPUT:
      strValue = CServiceBroker::GetPVRManager().GUIActions()->GetChannelNumberInputHandler().GetChannelNumberLabel();
      return true;
  }

  return false;
}

namespace
{
  std::string GetEpgEventTitle(const CPVREpgInfoTagPtr& epgTag)
  {
    if (epgTag)
      return epgTag->Title();
    else if (CServiceBroker::GetSettings().GetBool(CSettings::SETTING_EPG_HIDENOINFOAVAILABLE))
      return std::string();
    else
      return g_localizeStrings.Get(19055); // no information available
  }
} // unnamed namespace

bool CPVRGUIInfo::GetRadioRDSLabel(const CFileItem *item, const CGUIInfo &info, std::string &strValue) const
{
  const CPVRRadioRDSInfoTagPtr tag = item->GetPVRRadioRDSInfoTag();
  if (tag)
  {
    switch (info.m_info)
    {
      case RDS_CHANNEL_COUNTRY:
        strValue = tag->GetCountry();
        return true;
      case RDS_TITLE:
        strValue = tag->GetTitle();
        return true;
      case RDS_ARTIST:
        strValue = tag->GetArtist();
        return true;
      case RDS_BAND:
        strValue = tag->GetBand();
        return true;
      case RDS_COMPOSER:
        strValue = tag->GetComposer();
        return true;
      case RDS_CONDUCTOR:
        strValue = tag->GetConductor();
        return true;
      case RDS_ALBUM:
        strValue = tag->GetAlbum();
        return true;
      case RDS_ALBUM_TRACKNUMBER:
        if (tag->GetAlbumTrackNumber() > 0)
        {
          strValue = StringUtils::Format("%i", tag->GetAlbumTrackNumber());
          return true;
        }
        break;
      case RDS_GET_RADIO_STYLE:
        strValue = tag->GetRadioStyle();
        return true;
      case RDS_COMMENT:
        strValue = tag->GetComment();
        return true;
      case RDS_INFO_NEWS:
        strValue = tag->GetInfoNews();
        return true;
      case RDS_INFO_NEWS_LOCAL:
        strValue = tag->GetInfoNewsLocal();
        return true;
      case RDS_INFO_STOCK:
        strValue = tag->GetInfoStock();
        return true;
      case RDS_INFO_STOCK_SIZE:
        strValue = StringUtils::Format("%i", static_cast<int>(tag->GetInfoStock().size()));
        return true;
      case RDS_INFO_SPORT:
        strValue = tag->GetInfoSport();
        return true;
      case RDS_INFO_SPORT_SIZE:
        strValue = StringUtils::Format("%i", static_cast<int>(tag->GetInfoSport().size()));
        return true;
      case RDS_INFO_LOTTERY:
        strValue = tag->GetInfoLottery();
        return true;
      case RDS_INFO_LOTTERY_SIZE:
        strValue = StringUtils::Format("%i", static_cast<int>(tag->GetInfoLottery().size()));
        return true;
      case RDS_INFO_WEATHER:
        strValue = tag->GetInfoWeather();
        return true;
      case RDS_INFO_WEATHER_SIZE:
        strValue = StringUtils::Format("%i", static_cast<int>(tag->GetInfoWeather().size()));
        return true;
      case RDS_INFO_HOROSCOPE:
        strValue = tag->GetInfoHoroscope();
        return true;
      case RDS_INFO_HOROSCOPE_SIZE:
        strValue = StringUtils::Format("%i", static_cast<int>(tag->GetInfoHoroscope().size()));
        return true;
      case RDS_INFO_CINEMA:
        strValue = tag->GetInfoCinema();
        return true;
      case RDS_INFO_CINEMA_SIZE:
        strValue = StringUtils::Format("%i", static_cast<int>(tag->GetInfoCinema().size()));
        return true;
      case RDS_INFO_OTHER:
        strValue = tag->GetInfoOther();
        return true;
      case RDS_INFO_OTHER_SIZE:
        strValue = StringUtils::Format("%i", static_cast<int>(tag->GetInfoOther().size()));
        return true;
      case RDS_PROG_HOST:
        strValue = tag->GetProgHost();
        return true;
      case RDS_PROG_EDIT_STAFF:
        strValue = tag->GetEditorialStaff();
        return true;
      case RDS_PROG_HOMEPAGE:
        strValue = tag->GetProgWebsite();
        return true;
      case RDS_PROG_STYLE:
        strValue = tag->GetProgStyle();
        return true;
      case RDS_PHONE_HOTLINE:
        strValue = tag->GetPhoneHotline();
        return true;
      case RDS_PHONE_STUDIO:
        strValue = tag->GetPhoneStudio();
        return true;
      case RDS_SMS_STUDIO:
        strValue = tag->GetSMSStudio();
        return true;
      case RDS_EMAIL_HOTLINE:
        strValue = tag->GetEMailHotline();
        return true;
      case RDS_EMAIL_STUDIO:
        strValue = tag->GetEMailStudio();
        return true;
    }
  }

  switch (info.m_info)
  {
    case RDS_GET_RADIOTEXT_LINE:
      strValue = g_application.GetAppPlayer().GetRadioText(info.GetData1());
      return true;
    case RDS_PROG_STATION:
      if (tag)
        strValue = tag->GetProgStation();
      if (strValue.empty())
      {
        const CPVRChannelPtr channel = item->GetPVRChannelInfoTag();
        if (channel)
          strValue = channel->ChannelName();
      }
      return true;
    case RDS_PROG_NOW:
      if (tag)
        strValue = tag->GetProgNow();
      if (strValue.empty())
      {
        const CPVRChannelPtr channel = item->GetPVRChannelInfoTag();
        if (channel)
          strValue = GetEpgEventTitle(channel->GetEPGNow());
      }
      return true;
    case RDS_PROG_NEXT:
      if (tag)
        strValue = tag->GetProgNext();
      if (strValue.empty())
      {
        const CPVRChannelPtr channel = item->GetPVRChannelInfoTag();
        if (channel)
          strValue = GetEpgEventTitle(channel->GetEPGNext());
      }
      return true;
    case RDS_AUDIO_LANG:
      if (tag)
        strValue = tag->GetLanguage();
      if (strValue.empty())
      {
        AudioStreamInfo streamInfo;
        g_application.GetAppPlayer().GetAudioStreamInfo(g_application.GetAppPlayer().GetAudioStream(), streamInfo);
        strValue = streamInfo.language;
      }
      return true;
  }
  return false;
}

bool CPVRGUIInfo::GetInt(int& value, const CGUIListItem *item, int contextWindow, const CGUIInfo &info) const
{
  if (!item->IsFileItem())
    return false;

  const CFileItem *fitem = static_cast<const CFileItem*>(item);
  return GetListItemAndPlayerInt(fitem, info, value) ||
         GetPVRInt(fitem, info, value);
}

bool CPVRGUIInfo::GetListItemAndPlayerInt(const CFileItem *item, const CGUIInfo &info, int &iValue) const
{
  switch (info.m_info)
  {
    case LISTITEM_PROGRESS:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVREpgInfoTagPtr epgTag = CPVRItem(item).GetEpgInfoTag();
        if (epgTag)
          iValue = static_cast<int>(epgTag->ProgressPercentage());
      }
      return true;
  }
  return false;
}

bool CPVRGUIInfo::GetPVRInt(const CFileItem *item, const CGUIInfo &info, int& iValue) const
{
  CSingleLock lock(m_critSection);

  switch (info.m_info)
  {
    case PVR_EPG_EVENT_DURATION:
    {
      const CPVREpgInfoTagPtr epgTag = (item->IsPVRChannel() || item->IsEPG()) ? CPVRItem(item).GetEpgInfoTag() : nullptr;
      if (epgTag && epgTag != m_playingEpgTag)
        iValue = epgTag->GetDuration();
      else
        iValue = m_iDuration;
      return true;
    }
    case PVR_EPG_EVENT_PROGRESS:
    {
      const CPVREpgInfoTagPtr epgTag = (item->IsPVRChannel() || item->IsEPG()) ? CPVRItem(item).GetEpgInfoTag() : nullptr;
      if (epgTag && epgTag != m_playingEpgTag)
        iValue = std::lrintf(epgTag->ProgressPercentage());
      else
        iValue = std::lrintf(static_cast<float>(GetElapsedTime()) / m_iDuration * 100);
      return true;
    }
    case PVR_TIMESHIFT_PROGRESS:
      iValue = std::lrintf(static_cast<float>(m_iTimeshiftPlayTime - m_iTimeshiftStartTime) /
                           (m_iTimeshiftEndTime - m_iTimeshiftStartTime) * 100);
      return true;
    case PVR_ACTUAL_STREAM_SIG_PROGR:
      iValue = std::lrintf(static_cast<float>(m_qualityInfo.iSignal) / 0xFFFF * 100);
      return true;
    case PVR_ACTUAL_STREAM_SNR_PROGR:
      iValue = std::lrintf(static_cast<float>(m_qualityInfo.iSNR) / 0xFFFF * 100);
      return true;
    case PVR_BACKEND_DISKSPACE_PROGR:
      if (m_iBackendDiskTotal > 0)
        iValue = std::lrintf(static_cast<float>(m_iBackendDiskUsed) / m_iBackendDiskTotal * 100);
      else
        iValue = 0xFF;
      return true;
  }
  return false;
}

bool CPVRGUIInfo::GetBool(bool& value, const CGUIListItem *item, int contextWindow, const CGUIInfo &info) const
{
  if (!item->IsFileItem())
    return false;

  const CFileItem *fitem = static_cast<const CFileItem*>(item);
  return GetListItemAndPlayerBool(fitem, info, value) ||
         GetPVRBool(fitem, info, value) ||
         GetRadioRDSBool(fitem, info, value);
}

bool CPVRGUIInfo::GetListItemAndPlayerBool(const CFileItem *item, const CGUIInfo &info, bool &bValue) const
{
  switch (info.m_info)
  {
    case LISTITEM_ISRECORDING:
      if (item->IsPVRChannel())
      {
        bValue = item->GetPVRChannelInfoTag()->IsRecording();
        return true;
      }
      else if (item->IsEPG() || item->IsPVRTimer())
      {
        const CPVRTimerInfoTagPtr timer = CPVRItem(item).GetTimerInfoTag();
        if (timer)
          bValue = timer->IsRecording();
        return true;
      }
      else if (item->IsPVRRecording())
      {
        bValue = item->GetPVRRecordingInfoTag()->IsInProgress();
        return true;
      }
      break;
    case LISTITEM_INPROGRESS:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVREpgInfoTagPtr epgTag = CPVRItem(item).GetEpgInfoTag();
        if (epgTag)
          bValue = epgTag->IsActive();
        return true;
      }
      break;
    case LISTITEM_HASTIMER:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVREpgInfoTagPtr epgTag = CPVRItem(item).GetEpgInfoTag();
        if (epgTag)
          bValue = epgTag->HasTimer();
        return true;
      }
      break;
    case LISTITEM_HASTIMERSCHEDULE:
      if (item->IsPVRChannel() || item->IsEPG() || item->IsPVRTimer())
      {
        const CPVRTimerInfoTagPtr timer = CPVRItem(item).GetTimerInfoTag();
        if (timer)
          bValue = timer->GetTimerRuleId() != PVR_TIMER_NO_PARENT;
        return true;
      }
      break;
    case LISTITEM_TIMERISACTIVE:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVRTimerInfoTagPtr timer = CPVRItem(item).GetTimerInfoTag();
        if (timer)
          bValue = timer->IsActive();
        break;
      }
      break;
    case LISTITEM_TIMERHASCONFLICT:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVRTimerInfoTagPtr timer = CPVRItem(item).GetTimerInfoTag();
        if (timer)
          bValue = timer->HasConflict();
        return true;
      }
      break;
    case LISTITEM_TIMERHASERROR:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVRTimerInfoTagPtr timer = CPVRItem(item).GetTimerInfoTag();
        if (timer)
          bValue = (timer->IsBroken() && !timer->HasConflict());
        return true;
      }
      break;
    case LISTITEM_HASRECORDING:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVREpgInfoTagPtr epgTag = CPVRItem(item).GetEpgInfoTag();
        if (epgTag)
          bValue = epgTag->HasRecording();
        return true;
      }
      break;
    case LISTITEM_HAS_EPG:
      if (item->IsPVRChannel() || item->IsEPG() || item->IsPVRTimer())
      {
        const CPVREpgInfoTagPtr epgTag = CPVRItem(item).GetEpgInfoTag();
        bValue = (epgTag != nullptr);
        return true;
      }
      break;
    case LISTITEM_ISENCRYPTED:
      if (item->IsPVRChannel() || item->IsEPG())
      {
        const CPVRChannelPtr channel = CPVRItem(item).GetChannel();
        if (channel)
          bValue = channel->IsEncrypted();
        return true;
      }
      break;
    case MUSICPLAYER_CONTENT:
    case VIDEOPLAYER_CONTENT:
      if (item->IsPVRChannel())
      {
        bValue = StringUtils::EqualsNoCase(info.GetData3(), "livetv");
        return bValue; // if no match for this provider, other providers shall be asked.
      }
      break;
    case VIDEOPLAYER_HAS_INFO:
      if (item->IsPVRChannel())
      {
        bValue = !item->GetPVRChannelInfoTag()->IsEmpty();
        return true;
      }
      break;
    case VIDEOPLAYER_HAS_EPG:
      if (item->IsPVRChannel())
      {
        bValue = (item->GetPVRChannelInfoTag()->GetEPGNow() != nullptr);
        return true;
      }
      break;
    case VIDEOPLAYER_CAN_RESUME_LIVE_TV:
      if (item->IsPVRRecording())
      {
        const CPVRRecordingPtr recording = item->GetPVRRecordingInfoTag();
        const CPVREpgInfoTagPtr epgTag = CServiceBroker::GetPVRManager().EpgContainer().GetTagById(recording->Channel(), recording->BroadcastUid());
        bValue = (epgTag && epgTag->IsActive() && epgTag->Channel());
        return true;
      }
      break;
    case PLAYER_IS_CHANNEL_PREVIEW_ACTIVE:
      if (item->IsPVRChannel())
      {
        if (CServiceBroker::GetPVRManager().GUIActions()->GetChannelNavigator().IsPreviewAndShowInfo())
        {
          bValue = true;
        }
        else
        {
          bValue = !m_videoInfo.valid;
          if (bValue && item->GetPVRChannelInfoTag()->IsRadio())
            bValue = !m_audioInfo.valid;
        }
        return true;
      }
      break;
  }
  return false;
}

bool CPVRGUIInfo::GetPVRBool(const CFileItem *item, const CGUIInfo &info, bool& bValue) const
{
  CSingleLock lock(m_critSection);

  switch (info.m_info)
  {
    case PVR_IS_RECORDING:
      bValue = m_anyTimersInfo.HasRecordingTimers();
      return true;
    case PVR_IS_RECORDING_TV:
      bValue = m_tvTimersInfo.HasRecordingTimers();
      return true;
    case PVR_IS_RECORDING_RADIO:
      bValue = m_radioTimersInfo.HasRecordingTimers();
      return true;
    case PVR_HAS_TIMER:
      bValue = m_anyTimersInfo.HasTimers();
      return true;
    case PVR_HAS_TV_TIMER:
      bValue = m_tvTimersInfo.HasTimers();
      return true;
    case PVR_HAS_RADIO_TIMER:
      bValue = m_radioTimersInfo.HasTimers();
      return true;
    case PVR_HAS_TV_CHANNELS:
      bValue = m_bHasTVChannels;
      return true;
    case PVR_HAS_RADIO_CHANNELS:
      bValue = m_bHasRadioChannels;
      return true;
    case PVR_HAS_NONRECORDING_TIMER:
      bValue = m_anyTimersInfo.HasNonRecordingTimers();
      return true;
    case PVR_HAS_NONRECORDING_TV_TIMER:
      bValue = m_tvTimersInfo.HasNonRecordingTimers();
      return true;
    case PVR_HAS_NONRECORDING_RADIO_TIMER:
      bValue = m_radioTimersInfo.HasNonRecordingTimers();
      return true;
    case PVR_IS_PLAYING_TV:
      bValue = m_bIsPlayingTV;
      return true;
    case PVR_IS_PLAYING_RADIO:
      bValue = m_bIsPlayingRadio;
      return true;
    case PVR_IS_PLAYING_RECORDING:
      bValue = m_bIsPlayingRecording;
      return true;
    case PVR_IS_PLAYING_EPGTAG:
      bValue = m_bIsPlayingEpgTag;
      return true;
    case PVR_ACTUAL_STREAM_ENCRYPTED:
      bValue = m_bIsPlayingEncryptedStream;
      return true;
    case PVR_IS_TIMESHIFTING:
      bValue = m_bIsTimeshifting;
      return true;
    case PVR_CAN_RECORD_PLAYING_CHANNEL:
      bValue = m_bCanRecordPlayingChannel;
      return true;
    case PVR_IS_RECORDING_PLAYING_CHANNEL:
      bValue = m_bIsRecordingPlayingChannel;
      return true;
  }
  return false;
}

bool CPVRGUIInfo::GetRadioRDSBool(const CFileItem *item, const CGUIInfo &info, bool &bValue) const
{
  const CPVRRadioRDSInfoTagPtr tag = item->GetPVRRadioRDSInfoTag();
  if (tag)
  {
    switch (info.m_info)
    {
      case RDS_HAS_RADIOTEXT:
        bValue = tag->IsPlayingRadiotext();
        return true;
      case RDS_HAS_RADIOTEXT_PLUS:
        bValue = tag->IsPlayingRadiotextPlus();
        return true;
      case RDS_HAS_HOTLINE_DATA:
        bValue = (!tag->GetEMailHotline().empty() || !tag->GetPhoneHotline().empty());
        return true;
      case RDS_HAS_STUDIO_DATA:
        bValue = (!tag->GetEMailStudio().empty() || !tag->GetSMSStudio().empty() || !tag->GetPhoneStudio().empty());
        return true;
    }
  }

  switch (info.m_info)
  {
    case RDS_HAS_RDS:
      bValue = g_application.GetAppPlayer().IsPlayingRDS();
      return true;
  }

  return false;
}

namespace
{
  std::string TimeToTimeString(time_t datetime, TIME_FORMAT format, bool withSeconds)
  {
    CDateTime time;
    time.SetFromUTCDateTime(datetime);
    return time.GetAsLocalizedTime(format, withSeconds);
  }
} // unnamed namespace

void CPVRGUIInfo::CharInfoTimeshiftStartTime(TIME_FORMAT format, std::string &strValue) const
{
  strValue = TimeToTimeString(m_iTimeshiftStartTime, format, false);
}

void CPVRGUIInfo::CharInfoTimeshiftEndTime(TIME_FORMAT format, std::string &strValue) const
{
  strValue = TimeToTimeString(m_iTimeshiftEndTime, format, false);
}

void CPVRGUIInfo::CharInfoTimeshiftPlayTime(TIME_FORMAT format, std::string &strValue) const
{
  strValue = TimeToTimeString(m_iTimeshiftPlayTime, format, true);
}

void CPVRGUIInfo::CharInfoTimeshiftOffset(TIME_FORMAT format, std::string &strValue) const
{
  strValue = StringUtils::SecondsToTimeString(m_iTimeshiftOffset, format).c_str();
}

void CPVRGUIInfo::CharInfoEpgEventDuration(const CFileItem *item, TIME_FORMAT format, std::string &strValue) const
{
  int iDuration = 0;
  const CPVREpgInfoTagPtr epgTag = (item->IsPVRChannel() || item->IsEPG()) ? CPVRItem(item).GetEpgInfoTag() : nullptr;
  if (epgTag && epgTag != m_playingEpgTag)
    iDuration = epgTag->GetDuration();
  else
    iDuration = m_iDuration;

  strValue = StringUtils::SecondsToTimeString(iDuration, format).c_str();
}

void CPVRGUIInfo::CharInfoEpgEventElapsedTime(const CFileItem *item, TIME_FORMAT format, std::string &strValue) const
{
  int iElapsed = 0;
  const CPVREpgInfoTagPtr epgTag = (item->IsPVRChannel() || item->IsEPG()) ? CPVRItem(item).GetEpgInfoTag() : nullptr;
  if (epgTag && epgTag != m_playingEpgTag)
    iElapsed = epgTag->Progress();
  else
    iElapsed = GetElapsedTime();

  strValue = StringUtils::SecondsToTimeString(iElapsed, format).c_str();
}

int CPVRGUIInfo::GetRemainingTime(const CFileItem *item) const
{
  int iRemaining = 0;
  const CPVREpgInfoTagPtr epgTag = (item->IsPVRChannel() || item->IsEPG()) ? CPVRItem(item).GetEpgInfoTag() : nullptr;
  if (epgTag && epgTag != m_playingEpgTag)
    iRemaining = epgTag->GetDuration() - epgTag->Progress();
  else
    iRemaining = m_iDuration - GetElapsedTime();

  return iRemaining;
}

void CPVRGUIInfo::CharInfoEpgEventRemainingTime(const CFileItem *item, TIME_FORMAT format, std::string &strValue) const
{
  strValue = StringUtils::SecondsToTimeString(GetRemainingTime(item), format).c_str();
}

void CPVRGUIInfo::CharInfoEpgEventFinishTime(const CFileItem *item, TIME_FORMAT format, std::string &strValue) const
{
  CDateTime finish = CDateTime::GetCurrentDateTime();
  finish += CDateTimeSpan(0, 0, 0, GetRemainingTime(item));
  strValue = finish.GetAsLocalizedTime(format);
}

void CPVRGUIInfo::CharInfoBackendNumber(std::string &strValue) const
{
  size_t numBackends = m_backendProperties.size();

  if (numBackends > 0)
    strValue = StringUtils::Format("{0} {1} {2}", m_iCurrentActiveClient + 1, g_localizeStrings.Get(20163).c_str(), numBackends);
  else
    strValue = g_localizeStrings.Get(14023);
}

void CPVRGUIInfo::CharInfoTotalDiskSpace(std::string &strValue) const
{
  strValue = StringUtils::SizeToString(m_iBackendDiskTotal).c_str();
}

void CPVRGUIInfo::CharInfoSignal(std::string &strValue) const
{
  strValue = StringUtils::Format("%d %%", m_qualityInfo.iSignal / 655);
}

void CPVRGUIInfo::CharInfoSNR(std::string &strValue) const
{
  strValue = StringUtils::Format("%d %%", m_qualityInfo.iSNR / 655);
}

void CPVRGUIInfo::CharInfoBER(std::string &strValue) const
{
  strValue = StringUtils::Format("%08lX", m_qualityInfo.iBER);
}

void CPVRGUIInfo::CharInfoUNC(std::string &strValue) const
{
  strValue = StringUtils::Format("%08lX", m_qualityInfo.iUNC);
}

void CPVRGUIInfo::CharInfoFrontendName(std::string &strValue) const
{
  if (!strlen(m_qualityInfo.strAdapterName))
    strValue = g_localizeStrings.Get(13205);
  else
    strValue = m_qualityInfo.strAdapterName;
}

void CPVRGUIInfo::CharInfoFrontendStatus(std::string &strValue) const
{
  if (!strlen(m_qualityInfo.strAdapterStatus))
    strValue = g_localizeStrings.Get(13205);
  else
    strValue = m_qualityInfo.strAdapterStatus;
}

void CPVRGUIInfo::CharInfoBackendName(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;
  strValue = m_strBackendName;
}

void CPVRGUIInfo::CharInfoBackendVersion(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;
  strValue = m_strBackendVersion;
}

void CPVRGUIInfo::CharInfoBackendHost(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;
  strValue = m_strBackendHost;
}

void CPVRGUIInfo::CharInfoBackendDiskspace(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;

  auto diskTotal = m_iBackendDiskTotal;
  auto diskUsed = m_iBackendDiskUsed;

  if (diskTotal > 0)
  {
    strValue = StringUtils::Format(g_localizeStrings.Get(802).c_str(),
        StringUtils::SizeToString(diskTotal - diskUsed).c_str(),
        StringUtils::SizeToString(diskTotal).c_str());
  }
  else
    strValue = g_localizeStrings.Get(13205);
}

void CPVRGUIInfo::CharInfoBackendChannels(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;
  strValue = m_strBackendChannels;
}

void CPVRGUIInfo::CharInfoBackendTimers(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;
  strValue = m_strBackendTimers;
}

void CPVRGUIInfo::CharInfoBackendRecordings(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;
  strValue = m_strBackendRecordings;
}

void CPVRGUIInfo::CharInfoBackendDeletedRecordings(std::string &strValue) const
{
  m_updateBackendCacheRequested = true;
  strValue = m_strBackendDeletedRecordings;
}

void CPVRGUIInfo::CharInfoPlayingClientName(std::string &strValue) const
{
  if (m_strPlayingClientName.empty())
    strValue = g_localizeStrings.Get(13205);
  else
    strValue = m_strPlayingClientName;
}

void CPVRGUIInfo::CharInfoEncryption(std::string &strValue) const
{
  if (m_descrambleInfo.iCaid != PVR_DESCRAMBLE_INFO_NOT_AVAILABLE)
  {
    // prefer dynamically updated info, if available
    strValue = CPVRChannel::GetEncryptionName(m_descrambleInfo.iCaid);
    return;
  }
  else
  {
    const CPVRChannelPtr channel(CServiceBroker::GetPVRManager().GetPlayingChannel());
    if (channel)
    {
      strValue = channel->EncryptionName();
      return;
    }
  }

  strValue.clear();
}

void CPVRGUIInfo::CharInfoService(std::string &strValue) const
{
  if (!strlen(m_qualityInfo.strServiceName))
    strValue = g_localizeStrings.Get(13205);
  else
    strValue = m_qualityInfo.strServiceName;
}

void CPVRGUIInfo::CharInfoMux(std::string &strValue) const
{
  if (!strlen(m_qualityInfo.strMuxName))
    strValue = g_localizeStrings.Get(13205);
  else
    strValue = m_qualityInfo.strMuxName;
}

void CPVRGUIInfo::CharInfoProvider(std::string &strValue) const
{
  if (!strlen(m_qualityInfo.strProviderName))
    strValue = g_localizeStrings.Get(13205);
  else
    strValue = m_qualityInfo.strProviderName;
}

void CPVRGUIInfo::UpdateBackendCache(void)
{
  CSingleLock lock(m_critSection);

  // Update the backend information for all backends if
  // an update has been requested
  if (m_iCurrentActiveClient == 0 && m_updateBackendCacheRequested)
  {
    std::vector<SBackend> backendProperties;
    {
      CSingleExit exit(m_critSection);
      backendProperties = CServiceBroker::GetPVRManager().Clients()->GetBackendProperties();
    }

    m_backendProperties = backendProperties;
    m_updateBackendCacheRequested = false;
  }

  // Store some defaults
  m_strBackendName = g_localizeStrings.Get(13205);
  m_strBackendVersion = g_localizeStrings.Get(13205);
  m_strBackendHost = g_localizeStrings.Get(13205);
  m_strBackendChannels = g_localizeStrings.Get(13205);
  m_strBackendTimers = g_localizeStrings.Get(13205);
  m_strBackendRecordings = g_localizeStrings.Get(13205);
  m_strBackendDeletedRecordings = g_localizeStrings.Get(13205);
  m_iBackendDiskTotal = 0;
  m_iBackendDiskUsed = 0;

  // Update with values from the current client when we have at least one
  if (!m_backendProperties.empty())
  {
    const auto &backend = m_backendProperties[m_iCurrentActiveClient];

    m_strBackendName = backend.name;
    m_strBackendVersion = backend.version;
    m_strBackendHost = backend.host;

    if (backend.numChannels >= 0)
      m_strBackendChannels = StringUtils::Format("%i", backend.numChannels);

    if (backend.numTimers >= 0)
      m_strBackendTimers = StringUtils::Format("%i", backend.numTimers);

    if (backend.numRecordings >= 0)
      m_strBackendRecordings = StringUtils::Format("%i", backend.numRecordings);

    if (backend.numDeletedRecordings >= 0)
      m_strBackendDeletedRecordings = StringUtils::Format("%i", backend.numDeletedRecordings);

    m_iBackendDiskTotal = backend.diskTotal;
    m_iBackendDiskUsed = backend.diskUsed;
  }

  // Update the current active client, eventually wrapping around
  if (++m_iCurrentActiveClient >= m_backendProperties.size())
    m_iCurrentActiveClient = 0;
}

void CPVRGUIInfo::UpdateTimersCache(void)
{
  m_anyTimersInfo.UpdateTimersCache();
  m_tvTimersInfo.UpdateTimersCache();
  m_radioTimersInfo.UpdateTimersCache();
}

void CPVRGUIInfo::UpdateTimersToggle(void)
{
  m_anyTimersInfo.UpdateTimersToggle();
  m_tvTimersInfo.UpdateTimersToggle();
  m_radioTimersInfo.UpdateTimersToggle();
}

void CPVRGUIInfo::UpdateNextTimer(void)
{
  m_anyTimersInfo.UpdateNextTimer();
  m_tvTimersInfo.UpdateNextTimer();
  m_radioTimersInfo.UpdateNextTimer();
}

int CPVRGUIInfo::GetElapsedTime(void) const
{
  CSingleLock lock(m_critSection);

  if (m_playingEpgTag || m_iTimeshiftStartTime)
  {
    CDateTime current(m_iTimeshiftPlayTime);
    CDateTime start = m_playingEpgTag ? m_playingEpgTag->StartAsUTC()
                                      : CDateTime(m_iTimeshiftStartTime);
    CDateTimeSpan time = current > start ? current - start : CDateTimeSpan(0, 0, 0, 0);
    return time.GetSecondsTotal();
  }
  else
  {
    return 0;
  }
}

void CPVRGUIInfo::ResetPlayingTag(void)
{
  CSingleLock lock(m_critSection);
  m_playingEpgTag.reset();
  m_iDuration = 0;
}

CPVREpgInfoTagPtr CPVRGUIInfo::GetPlayingTag() const
{
  CSingleLock lock(m_critSection);
  return m_playingEpgTag;
}

void CPVRGUIInfo::UpdatePlayingTag(void)
{
  const CPVRChannelPtr currentChannel(CServiceBroker::GetPVRManager().GetPlayingChannel());
  const CPVREpgInfoTagPtr currentTag(CServiceBroker::GetPVRManager().GetPlayingEpgTag());
  if (currentChannel || currentTag)
  {
    CPVREpgInfoTagPtr epgTag(GetPlayingTag());
    CPVRChannelPtr channel;
    if (epgTag)
      channel = epgTag->Channel();

    if (!epgTag || !epgTag->IsActive() ||
        !channel || !currentChannel || *channel != *currentChannel)
    {
      const CPVREpgInfoTagPtr newTag(currentTag ? currentTag : currentChannel->GetEPGNow());

      CSingleLock lock(m_critSection);
      if (newTag)
      {
        m_playingEpgTag = newTag;
        m_iDuration = m_playingEpgTag->GetDuration();
      }
      else if (m_iTimeshiftEndTime > m_iTimeshiftStartTime)
      {
        m_playingEpgTag.reset();
        m_iDuration = m_iTimeshiftEndTime - m_iTimeshiftStartTime;
      }
      else
      {
        m_playingEpgTag.reset();
        m_iDuration = 0;
      }
    }
  }
  else
  {
    const CPVRRecordingPtr recording(CServiceBroker::GetPVRManager().GetPlayingRecording());
    if (recording)
    {
      CSingleLock lock(m_critSection);
      m_playingEpgTag.reset();
      m_iDuration = recording->GetDuration();
    }
  }
}
