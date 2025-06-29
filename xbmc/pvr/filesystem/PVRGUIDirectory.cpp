/*
 *  Copyright (C) 2012-2019 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PVRGUIDirectory.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "GUIUserMessages.h"
#include "ServiceBroker.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/WindowIDs.h"
#include "input/WindowTranslator.h"
#include "pvr/PVRConstants.h" // PVR_CLIENT_INVALID_UID
#include "pvr/PVRManager.h"
#include "pvr/PVRPlaybackState.h"
#include "pvr/addons/PVRClient.h"
#include "pvr/addons/PVRClients.h"
#include "pvr/channels/PVRChannel.h"
#include "pvr/channels/PVRChannelGroupMember.h"
#include "pvr/channels/PVRChannelGroups.h"
#include "pvr/channels/PVRChannelGroupsContainer.h"
#include "pvr/channels/PVRChannelsPath.h"
#include "pvr/epg/EpgContainer.h"
#include "pvr/epg/EpgSearch.h"
#include "pvr/epg/EpgSearchFilter.h"
#include "pvr/epg/EpgSearchPath.h"
#include "pvr/providers/PVRProvider.h"
#include "pvr/providers/PVRProviders.h"
#include "pvr/providers/PVRProvidersPath.h"
#include "pvr/recordings/PVRRecording.h"
#include "pvr/recordings/PVRRecordings.h"
#include "pvr/recordings/PVRRecordingsPath.h"
#include "pvr/timers/PVRTimerInfoTag.h"
#include "pvr/timers/PVRTimers.h"
#include "pvr/timers/PVRTimersPath.h"
#include "pvr/utils/PVRPathUtils.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/Job.h"
#include "utils/JobManager.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace PVR;

bool CPVRGUIDirectory::Exists() const
{
  if (!CServiceBroker::GetPVRManager().IsStarted())
    return false;

  return m_url.IsProtocol("pvr") && StringUtils::StartsWith(m_url.GetFileName(), "recordings");
}

bool CPVRGUIDirectory::SupportsWriteFileOperations() const
{
  if (!CServiceBroker::GetPVRManager().IsStarted())
    return false;

  const std::string filename = m_url.GetFileName();
  return URIUtils::IsPVRRecording(filename);
}

namespace
{
void ResolveNonPVRItem(CFileItem& item)
{
  if (URIUtils::IsPVRChannel(item.GetDynPath()))
  {
    const std::shared_ptr<CPVRChannelGroupMember> groupMember{
        CServiceBroker::GetPVRManager().ChannelGroups()->GetChannelGroupMemberByPath(
            item.GetDynPath())};
    if (groupMember)
      item = CFileItem(groupMember); // Replace original item with a PVR channel item
  }
  else if (URIUtils::IsPVRRecording(item.GetDynPath()))
  {
    const std::shared_ptr<CPVRRecording> recording{
        CServiceBroker::GetPVRManager().Recordings()->GetByPath(item.GetDynPath())};
    if (recording)
      item = CFileItem(recording); // Replace original item with a PVR recording item
  }
  else if (URIUtils::IsPVRGuideItem(item.GetDynPath()))
  {
    const std::shared_ptr<CPVREpgInfoTag> epgTag{
        CServiceBroker::GetPVRManager().EpgContainer().GetTagByPath(item.GetDynPath())};
    if (epgTag)
      item = CFileItem(epgTag); // Replace original item with a PVR EPG tag item
  }
  else
  {
    CLog::LogF(LOGWARNING, "Unhandled item ({}).", item.GetDynPath());
  }
}
} // unnamed namespace

bool CPVRGUIDirectory::Resolve(CFileItem& item)
{
  // Item passed in could be carrying a plugin URL as path and a PVR channel URL as dyn path
  // for example. We need to resolve those items to PVR items carrying a PVR URL as path before
  // we can continue.
  if (!URIUtils::IsPVR(item.GetPath()))
  {
    if (URIUtils::IsPVR(item.GetDynPath()))
      ResolveNonPVRItem(item);
    else
      return false; // Neither path nor dyn path contain a PVR URL. Not resolvable here.
  }
  return CServiceBroker::GetPVRManager().PlaybackState()->OnPreparePlayback(item);
}

namespace
{

bool GetRootDirectory(bool bRadio, CFileItemList& results)
{
  std::shared_ptr<CFileItem> item;

  const std::shared_ptr<const CPVRClients> clients = CServiceBroker::GetPVRManager().Clients();

  // EPG
  const bool bAnyClientSupportingEPG = clients->AnyClientSupportingEPG();
  if (bAnyClientSupportingEPG)
  {
    item = std::make_shared<CFileItem>(
        StringUtils::Format("pvr://guide/{}/", bRadio ? "radio" : "tv"), true);
    item->SetLabel(g_localizeStrings.Get(19069)); // Guide
    item->SetProperty("node.target", CWindowTranslator::TranslateWindow(bRadio ? WINDOW_RADIO_GUIDE
                                                                               : WINDOW_TV_GUIDE));
    item->SetArt("icon", "DefaultPVRGuide.png");
    results.Add(item);
  }

  // Channels
  item = std::make_shared<CFileItem>(
      bRadio ? CPVRChannelsPath::PATH_RADIO_CHANNELS : CPVRChannelsPath::PATH_TV_CHANNELS, true);
  item->SetLabel(g_localizeStrings.Get(19019)); // Channels
  item->SetProperty("node.target", CWindowTranslator::TranslateWindow(bRadio ? WINDOW_RADIO_CHANNELS
                                                                             : WINDOW_TV_CHANNELS));
  item->SetArt("icon", "DefaultPVRChannels.png");
  results.Add(item);

  // Recordings
  if (clients->AnyClientSupportingRecordings())
  {
    item = std::make_shared<CFileItem>(bRadio ? CPVRRecordingsPath::PATH_ACTIVE_RADIO_RECORDINGS
                                              : CPVRRecordingsPath::PATH_ACTIVE_TV_RECORDINGS,
                                       true);
    item->SetLabel(g_localizeStrings.Get(19017)); // Recordings
    item->SetProperty("node.target", CWindowTranslator::TranslateWindow(
                                         bRadio ? WINDOW_RADIO_RECORDINGS : WINDOW_TV_RECORDINGS));
    item->SetArt("icon", "DefaultPVRRecordings.png");
    results.Add(item);
  }

  // Providers
  if (CServiceBroker::GetPVRManager().Providers()->GetNumProviders() > 1)
  {
    item = std::make_shared<CFileItem>(bRadio ? CPVRProvidersPath::PATH_RADIO_PROVIDERS
                                              : CPVRProvidersPath::PATH_TV_PROVIDERS,
                                       true);
    item->SetLabel(g_localizeStrings.Get(19334)); // Providers
    item->SetProperty("node.target", CWindowTranslator::TranslateWindow(
                                         bRadio ? WINDOW_RADIO_PROVIDERS : WINDOW_TV_PROVIDERS));
    item->SetArt("icon", "DefaultPVRProviders.png");
    results.Add(std::move(item));
  }

  // Timers/Timer rules
  // - always present, because Reminders are always available, no client support needed for this
  item = std::make_shared<CFileItem>(
      bRadio ? CPVRTimersPath::PATH_RADIO_TIMERS : CPVRTimersPath::PATH_TV_TIMERS, true);
  item->SetLabel(g_localizeStrings.Get(19040)); // Timers
  item->SetProperty("node.target", CWindowTranslator::TranslateWindow(bRadio ? WINDOW_RADIO_TIMERS
                                                                             : WINDOW_TV_TIMERS));
  item->SetArt("icon", "DefaultPVRTimers.png");
  results.Add(item);

  item = std::make_shared<CFileItem>(
      bRadio ? CPVRTimersPath::PATH_RADIO_TIMER_RULES : CPVRTimersPath::PATH_TV_TIMER_RULES, true);
  item->SetLabel(g_localizeStrings.Get(19138)); // Timer rules
  item->SetProperty("node.target", CWindowTranslator::TranslateWindow(
                                       bRadio ? WINDOW_RADIO_TIMER_RULES : WINDOW_TV_TIMER_RULES));
  item->SetArt("icon", "DefaultPVRTimerRules.png");
  results.Add(item);

  // Search
  if (bAnyClientSupportingEPG)
  {
    item = std::make_shared<CFileItem>(
        bRadio ? CPVREpgSearchPath::PATH_RADIO_SEARCH : CPVREpgSearchPath::PATH_TV_SEARCH, true);
    item->SetLabel(g_localizeStrings.Get(137)); // Search
    item->SetProperty("node.target", CWindowTranslator::TranslateWindow(bRadio ? WINDOW_RADIO_SEARCH
                                                                               : WINDOW_TV_SEARCH));
    item->SetArt("icon", "DefaultPVRSearch.png");
    results.Add(item);
  }

  return true;
}

} // unnamed namespace

bool CPVRGUIDirectory::GetDirectory(CFileItemList& results) const
{
  std::string base = m_url.Get();
  URIUtils::RemoveSlashAtEnd(base);

  std::string fileName = m_url.GetFileName();
  URIUtils::RemoveSlashAtEnd(fileName);

  results.SetCacheToDisc(CFileItemList::CacheType::NEVER);

  if (fileName.empty())
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      std::shared_ptr<CFileItem> item;

      item = std::make_shared<CFileItem>(base + "channels/", true);
      item->SetLabel(g_localizeStrings.Get(19019)); // Channels
      item->SetLabelPreformatted(true);
      results.Add(item);

      item = std::make_shared<CFileItem>(base + "recordings/active/", true);
      item->SetLabel(g_localizeStrings.Get(19017)); // Recordings
      item->SetLabelPreformatted(true);
      results.Add(item);

      item = std::make_shared<CFileItem>(base + "recordings/deleted/", true);
      item->SetLabel(g_localizeStrings.Get(19184)); // Deleted recordings
      item->SetLabelPreformatted(true);
      results.Add(item);

      // Sort by name only. Labels are preformatted.
      results.AddSortMethod(SortByLabel, 551 /* Name */, LABEL_MASKS("%L", "", "%L", ""));
    }
    return true;
  }
  else if (StringUtils::StartsWith(fileName, "tv"))
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      return GetRootDirectory(false, results);
    }
    return true;
  }
  else if (StringUtils::StartsWith(fileName, "radio"))
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      return GetRootDirectory(true, results);
    }
    return true;
  }
  else if (StringUtils::StartsWith(fileName, "recordings"))
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      return GetRecordingsDirectory(results);
    }
    return true;
  }
  else if (StringUtils::StartsWith(fileName, "channels"))
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      return GetChannelsDirectory(results);
    }
    return true;
  }
  else if (StringUtils::StartsWith(fileName, "providers"))
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      return GetProvidersDirectory(results);
    }
    return true;
  }
  else if (StringUtils::StartsWith(fileName, "timers"))
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      return GetTimersDirectory(results);
    }
    return true;
  }

  const CPVREpgSearchPath path(m_url.Get());
  if (path.IsValid())
  {
    if (CServiceBroker::GetPVRManager().IsStarted())
    {
      if (path.IsSavedSearchesRoot())
        return GetSavedSearchesDirectory(path.IsRadio(), results);
      else if (path.IsSavedSearch())
        return GetSavedSearchResults(path.IsRadio(), path.GetId(), results);
    }
    return true;
  }

  return false;
}

bool CPVRGUIDirectory::HasTVRecordings()
{
  return CServiceBroker::GetPVRManager().IsStarted() &&
         CServiceBroker::GetPVRManager().Recordings()->GetNumTVRecordings() > 0;
}

bool CPVRGUIDirectory::HasDeletedTVRecordings()
{
  return CServiceBroker::GetPVRManager().IsStarted() &&
         CServiceBroker::GetPVRManager().Recordings()->HasDeletedTVRecordings();
}

bool CPVRGUIDirectory::HasRadioRecordings()
{
  return CServiceBroker::GetPVRManager().IsStarted() &&
         CServiceBroker::GetPVRManager().Recordings()->GetNumRadioRecordings() > 0;
}

bool CPVRGUIDirectory::HasDeletedRadioRecordings()
{
  return CServiceBroker::GetPVRManager().IsStarted() &&
         CServiceBroker::GetPVRManager().Recordings()->HasDeletedRadioRecordings();
}

namespace
{

std::string TrimSlashes(const std::string& strOrig)
{
  std::string strReturn = strOrig;
  while (strReturn[0] == '/')
    strReturn.erase(0, 1);

  URIUtils::RemoveSlashAtEnd(strReturn);
  return strReturn;
}

bool IsDirectoryMember(const std::string& strDirectory,
                       const std::string& strEntryDirectory,
                       bool bGrouped)
{
  const std::string strUseDirectory = TrimSlashes(strDirectory);
  const std::string strUseEntryDirectory = TrimSlashes(strEntryDirectory);

  // Case-insensitive comparison since sub folders are created with case-insensitive matching (GetSubDirectories)
  if (bGrouped)
    return StringUtils::EqualsNoCase(strUseDirectory, strUseEntryDirectory);
  else
    return StringUtils::StartsWithNoCase(strUseEntryDirectory, strUseDirectory);
}

template<class T>
class CByClientAndProviderFilter
{
public:
  explicit CByClientAndProviderFilter(const CURL& url)
    : m_applyFilter(UTILS::GetClientAndProviderFromPath(url, m_clientId, m_providerId))
  {
  }

  bool Filter(const std::shared_ptr<T>& item) const
  {
    if (m_applyFilter)
    {
      if (item->ClientID() != m_clientId)
        return true;

      if ((m_providerId != PVR_PROVIDER_INVALID_UID) && (item->ClientProviderUid() != m_providerId))
        return true;
    }
    return false;
  }

private:
  int m_clientId{PVR_CLIENT_INVALID_UID};
  int m_providerId{PVR_PROVIDER_INVALID_UID};
  bool m_applyFilter{false};
};

template<typename T>
void GetGetRecordingsSubDirectories(const CPVRRecordingsPath& recParentPath,
                                    const std::vector<std::shared_ptr<CPVRRecording>>& recordings,
                                    const CByClientAndProviderFilter<T>& byClientAndProviderFilter,
                                    CFileItemList& results)
{
  // Only active recordings are fetched to provide sub directories.
  // Not applicable for deleted view which is supposed to be flattened.
  std::set<std::shared_ptr<CFileItem>> unwatchedFolders;
  bool bRadio = recParentPath.IsRadio();

  for (const auto& recording : recordings)
  {
    if (byClientAndProviderFilter.Filter(recording))
      continue;

    if (recording->IsDeleted())
      continue;

    if (recording->IsRadio() != bRadio)
      continue;

    const std::string strCurrent =
        recParentPath.GetUnescapedSubDirectoryPath(recording->Directory());
    if (strCurrent.empty())
      continue;

    CPVRRecordingsPath recChildPath(recParentPath);
    recChildPath.AppendSegment(strCurrent);
    const std::string strFilePath{recChildPath.AsString()};

    std::shared_ptr<CFileItem> item;
    if (!results.Contains(strFilePath))
    {
      item = std::make_shared<CFileItem>(strCurrent, true);
      item->SetPath(strFilePath);
      item->SetLabel(strCurrent);
      item->SetLabelPreformatted(true);
      item->SetDateTime(recording->RecordingTimeAsLocalTime());
      item->SetProperty("totalepisodes", 0);
      item->SetProperty("watchedepisodes", 0);
      item->SetProperty("unwatchedepisodes", 0);
      item->SetProperty("inprogressepisodes", 0);
      item->SetProperty("sizeinbytes", UINT64_C(0));

      // Assume all folders are watched, we'll change the overlay later
      item->SetOverlayImage(CGUIListItem::ICON_OVERLAY_WATCHED);
      results.Add(item);
    }
    else
    {
      item = results.Get(strFilePath);
      if (item->GetDateTime() < recording->RecordingTimeAsLocalTime())
        item->SetDateTime(recording->RecordingTimeAsLocalTime());
    }

    item->IncrementProperty("totalepisodes", 1);
    if (recording->GetPlayCount() == 0)
    {
      unwatchedFolders.insert(item);
      item->IncrementProperty("unwatchedepisodes", 1);
    }
    else
    {
      item->IncrementProperty("watchedepisodes", 1);
    }
    // Note: Calling GetResumePoint() could involve a PVR add-on backend call!
    // So we fetch the the locally cached resume point here for performance reasons.
    if (recording->GetLocalResumePoint().IsPartWay())
    {
      item->IncrementProperty("inprogressepisodes", 1);
    }
    item->IncrementProperty("sizeinbytes", recording->GetSizeInBytes());
  }

  // Replace the incremental size of the recordings with a string equivalent
  for (auto& item : results.GetList())
  {
    int64_t size = item->GetProperty("sizeinbytes").asInteger();
    item->ClearProperty("sizeinbytes");
    item->SetSize(size); // We'll also sort recording folders by size
    if (size > 0)
      item->SetProperty("recordingsize", StringUtils::SizeToString(size));
  }

  // Change the watched overlay to unwatched for folders containing unwatched entries
  for (auto& item : unwatchedFolders)
    item->SetOverlayImage(CGUIListItem::ICON_OVERLAY_UNWATCHED);
}

} // unnamed namespace

bool CPVRGUIDirectory::GetRecordingsDirectoryInfo(CFileItem& item)
{
  CFileItemList results;
  const CPVRGUIDirectory dir{item.GetPath()};
  if (dir.GetRecordingsDirectory(results))
  {
    item.SetLabelPreformatted(true);
    item.SetProperty("totalepisodes", 0);
    item.SetProperty("watchedepisodes", 0);
    item.SetProperty("unwatchedepisodes", 0);
    item.SetProperty("inprogressepisodes", 0);

    int64_t sizeInBytes{0};

    for (const auto& result : results.GetList())
    {
      const auto recording{result->GetPVRRecordingInfoTag()};
      if (!recording)
        continue;

      const CDateTime& dateTime{item.GetDateTime()};
      if (dateTime.IsValid() || (dateTime < recording->RecordingTimeAsLocalTime()))
        item.SetDateTime(recording->RecordingTimeAsLocalTime());

      item.IncrementProperty("totalepisodes", 1);

      if (recording->GetPlayCount() == 0)
        item.IncrementProperty("unwatchedepisodes", 1);
      else
        item.IncrementProperty("watchedepisodes", 1);

      if (recording->GetResumePoint().IsPartWay())
        item.IncrementProperty("inprogressepisodes", 1);

      sizeInBytes += recording->GetSizeInBytes();
    }

    item.SetProperty("recordingsize", StringUtils::SizeToString(sizeInBytes));

    if (item.GetProperty("unwatchedepisodes").asInteger() > 0)
      item.SetOverlayImage(CGUIListItem::ICON_OVERLAY_UNWATCHED);
    else
      item.SetOverlayImage(CGUIListItem::ICON_OVERLAY_WATCHED);

    return true;
  }
  return false;
}

namespace
{
class CPVRRecordingFoldersInProgressEpisodesCountJob : public CJob
{
public:
  CPVRRecordingFoldersInProgressEpisodesCountJob(
      const CFileItemList& folders, const std::vector<std::shared_ptr<CPVRRecording>>& recordings)
    : m_recordings(recordings)
  {
    // Take a copy of original items; do not manipulate them directly (CFileItem is not threadsafe).
    m_folders.Copy(folders);
  }

  bool DoWork() override
  {
    if (m_recordings.empty() || m_folders.IsEmpty())
      return true; // Nothing to do.

    auto& windowMgr{CServiceBroker::GetGUI()->GetWindowManager()};

    for (const auto& folder : m_folders)
    {
      const CPVRRecordingsPath recPath{folder->GetPath()};
      if (!recPath.IsValid())
        continue;

      const auto oldInProgressEpisodes{folder->GetProperty("inprogressepisodes").asInteger(0)};

      // Get all matching recordings of the current directory and sum up in-progress episodes.
      int inProgressEpisodes{0};
      const CByClientAndProviderFilter<CPVRRecording> byClientAndProviderFilter{folder->GetURL()};
      const std::string directory{recPath.GetUnescapedDirectoryPath()};
      for (const auto& recording : m_recordings)
      {
        // Omit recordings not matching criteria.
        if (recording->IsDeleted() != recPath.IsDeleted() ||
            recording->IsRadio() != recPath.IsRadio() ||
            byClientAndProviderFilter.Filter(recording) ||
            !IsDirectoryMember(directory, recording->Directory(), true))
          continue;

        // Note: This could involve a PVR add-on backend call!
        if (recording->GetResumePoint().IsPartWay())
          inProgressEpisodes++;
      }

      if (inProgressEpisodes != oldInProgressEpisodes)
      {
        folder->SetProperty("inprogressepisodes", inProgressEpisodes);
        windowMgr.SendThreadMessage(
            {GUI_MSG_NOTIFY_ALL, windowMgr.GetActiveWindow(), 0, GUI_MSG_UPDATE_ITEM, 0, folder});
      }
    }
    return true;
  }

private:
  CFileItemList m_folders;
  std::vector<std::shared_ptr<CPVRRecording>> m_recordings;
};
} // unnamed namespace

bool CPVRGUIDirectory::GetRecordingsDirectory(CFileItemList& results) const
{
  results.SetContent("recordings");

  bool bGrouped = false;
  const std::vector<std::shared_ptr<CPVRRecording>> recordings =
      CServiceBroker::GetPVRManager().Recordings()->GetAll();

  if (m_url.HasOption("view"))
  {
    const std::string view = m_url.GetOption("view");
    if (view == "grouped")
      bGrouped = true;
    else if (view == "flat")
      bGrouped = false;
    else
    {
      CLog::LogF(LOGERROR, "Unsupported value '{}' for url parameter 'view'", view);
      return false;
    }
  }
  else
  {
    bGrouped = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
        CSettings::SETTING_PVRRECORD_GROUPRECORDINGS);
  }

  const CPVRRecordingsPath recPath(m_url.GetWithoutOptions());
  if (recPath.IsValid())
  {
    // Filter by client id/provider id ?
    const CByClientAndProviderFilter<CPVRRecording> byClientAndProviderFilter{m_url};

    // Get the directory structure if in non-flatten mode
    // Deleted view is always flatten. So only for an active view
    const std::string strDirectory = recPath.GetUnescapedDirectoryPath();
    if (!recPath.IsDeleted() && bGrouped)
      GetGetRecordingsSubDirectories(recPath, recordings, byClientAndProviderFilter, results);

    if (!results.IsEmpty())
    {
      // Update folders in-progress episodes count asynchronously, as this can involve
      // many PVR backend calls (one per recording), due to PVR add-on API limitations.
      CServiceBroker::GetJobManager()->AddJob(
          new CPVRRecordingFoldersInProgressEpisodesCountJob(results, recordings), nullptr);
    }

    // get all files of the current directory or recursively all files starting at the current directory if in flatten mode
    std::shared_ptr<CFileItem> item;
    for (const auto& recording : recordings)
    {
      // Omit recordings not matching criteria

      if (byClientAndProviderFilter.Filter(recording))
        continue;

      if (recording->IsDeleted() != recPath.IsDeleted() ||
          recording->IsRadio() != recPath.IsRadio() ||
          !IsDirectoryMember(strDirectory, recording->Directory(), bGrouped))
        continue;

      item = std::make_shared<CFileItem>(recording);
      item->SetOverlayImage(recording->GetPlayCount() > 0 ? CGUIListItem::ICON_OVERLAY_WATCHED
                                                          : CGUIListItem::ICON_OVERLAY_UNWATCHED);
      results.Add(item);
    }
  }

  return recPath.IsValid();
}

bool CPVRGUIDirectory::GetSavedSearchesDirectory(bool bRadio, CFileItemList& results) const
{
  const std::vector<std::shared_ptr<CPVREpgSearchFilter>> searches =
      CServiceBroker::GetPVRManager().EpgContainer().GetSavedSearches(bRadio);

  for (const auto& search : searches)
  {
    results.Add(std::make_shared<CFileItem>(search));
  }
  return true;
}

bool CPVRGUIDirectory::GetSavedSearchResults(bool isRadio, int id, CFileItemList& results) const
{
  const auto& epgContainer{CServiceBroker::GetPVRManager().EpgContainer()};
  const std::shared_ptr<CPVREpgSearchFilter> filter{epgContainer.GetSavedSearchById(isRadio, id)};
  if (filter)
  {
    CPVREpgSearch search(*filter);
    search.Execute();
    const auto& tags{search.GetResults()};
    for (const auto& tag : tags)
    {
      results.Add(std::make_shared<CFileItem>(tag));
    }
    return true;
  }
  return false;
}

bool CPVRGUIDirectory::GetChannelGroupsDirectory(bool bRadio,
                                                 bool bExcludeHidden,
                                                 CFileItemList& results)
{
  const std::shared_ptr<const CPVRChannelGroups> channelGroups{
      CServiceBroker::GetPVRManager().ChannelGroups()->Get(bRadio)};
  if (channelGroups)
  {
    std::shared_ptr<CFileItem> item;
    const std::vector<std::shared_ptr<CPVRChannelGroup>> groups =
        channelGroups->GetMembers(bExcludeHidden);
    for (const auto& group : groups)
    {
      item = std::make_shared<CFileItem>(group->GetPath().AsString(), true);
      item->SetTitle(group->GroupName());
      item->SetLabel(group->GroupName());
      results.Add(item);
    }
    return true;
  }
  return false;
}

namespace
{
std::shared_ptr<CPVRChannelGroupMember> GetLastWatchedChannelGroupMember(
    const std::shared_ptr<CPVRChannel>& channel)
{
  const int lastGroupId{channel->LastWatchedGroupId()};
  if (lastGroupId != PVR_GROUP_ID_UNNKOWN)
  {
    const std::shared_ptr<const CPVRChannelGroup> lastGroup{
        CServiceBroker::GetPVRManager().ChannelGroups()->GetByIdFromAll(lastGroupId)};
    if (lastGroup && !lastGroup->IsHidden() && !lastGroup->IsDeleted())
      return lastGroup->GetByUniqueID(channel->StorageId());
  }
  return {};
}

std::shared_ptr<CPVRChannelGroupMember> GetFirstMatchingGroupMember(
    const std::shared_ptr<CPVRChannel>& channel)
{
  const std::shared_ptr<const CPVRChannelGroups> groups{
      CServiceBroker::GetPVRManager().ChannelGroups()->Get(channel->IsRadio())};
  if (groups)
  {
    const std::vector<std::shared_ptr<CPVRChannelGroup>> channelGroups{
        groups->GetMembers(true /* exclude hidden */)};

    for (const auto& channelGroup : channelGroups)
    {
      if (channelGroup->IsDeleted())
        continue;

      std::shared_ptr<CPVRChannelGroupMember> groupMember{
          channelGroup->GetByUniqueID(channel->StorageId())};
      if (groupMember)
        return groupMember;
    }
  }
  return {};
}

std::vector<std::shared_ptr<CPVRChannelGroupMember>> GetChannelGroupMembers(
    const CPVRChannelsPath& path)
{
  const std::string& groupName{path.GetGroupName()};

  std::shared_ptr<CPVRChannelGroup> group;
  if (path.IsHiddenChannelGroup()) // hidden channels from the 'all channels' group
  {
    group = CServiceBroker::GetPVRManager().ChannelGroups()->GetGroupAll(path.IsRadio());
  }
  else if (groupName == "*") // all channels across all groups
  {
    group = CServiceBroker::GetPVRManager().ChannelGroups()->GetGroupAll(path.IsRadio());
    if (group)
    {
      std::vector<std::shared_ptr<CPVRChannelGroupMember>> result;

      const std::vector<std::shared_ptr<CPVRChannelGroupMember>> allGroupMembers{
          group->GetMembers(CPVRChannelGroup::Include::ONLY_VISIBLE)};
      for (const auto& allGroupMember : allGroupMembers)
      {
        std::shared_ptr<CPVRChannelGroupMember> member{
            GetLastWatchedChannelGroupMember(allGroupMember->Channel())};
        if (member)
        {
          result.emplace_back(member);
          continue; // Process next 'All channels' group member.
        }

        if (group->IsHidden())
        {
          // Very special case. 'All channels' group is hidden. Let's see what we get iterating all
          // non-hidden / non-deleted groups. We must not return any 'All channels' group members,
          // because their path is invalid (it contains the group).
          member = GetFirstMatchingGroupMember(allGroupMember->Channel());
          if (member)
            result.emplace_back(member);
        }
        else
        {
          // Use the 'All channels' group member.
          result.emplace_back(allGroupMember);
        }
      }
      return result;
    }
  }
  else
  {
    group = CServiceBroker::GetPVRManager()
                .ChannelGroups()
                ->Get(path.IsRadio())
                ->GetByName(groupName, path.GetGroupClientID());
  }

  if (group)
    return group->GetMembers(CPVRChannelGroup::Include::ALL);

  CLog::LogF(LOGERROR, "Unable to obtain members for channel group '{}'", groupName);
  return {};
}
} // unnamed namespace

bool CPVRGUIDirectory::GetChannelsDirectory(CFileItemList& results) const
{
  const CPVRChannelsPath path(m_url.GetWithoutOptions());
  if (path.IsValid())
  {
    if (path.IsEmpty())
    {
      std::shared_ptr<CFileItem> item;

      // all tv channels
      item = std::make_shared<CFileItem>(CPVRChannelsPath::PATH_TV_CHANNELS, true);
      item->SetLabel(g_localizeStrings.Get(19020)); // TV
      item->SetLabelPreformatted(true);
      results.Add(item);

      // all radio channels
      item = std::make_shared<CFileItem>(CPVRChannelsPath::PATH_RADIO_CHANNELS, true);
      item->SetLabel(g_localizeStrings.Get(19021)); // Radio
      item->SetLabelPreformatted(true);
      results.Add(item);

      return true;
    }
    else if (path.IsChannelsRoot())
    {
      return GetChannelGroupsDirectory(path.IsRadio(), true, results);
    }
    else if (path.IsChannelGroup())
    {
      const CByClientAndProviderFilter<const CPVRChannel> byClientAndProviderFilter{m_url};
      const bool playedOnly{(m_url.HasOption("view") && (m_url.GetOption("view") == "lastplayed"))};
      const bool dateAdded{(m_url.HasOption("view") && (m_url.GetOption("view") == "dateadded"))};
      const bool showHiddenChannels{path.IsHiddenChannelGroup()};
      const std::vector<std::shared_ptr<CPVRChannelGroupMember>> groupMembers{
          GetChannelGroupMembers(path)};
      for (const auto& groupMember : groupMembers)
      {
        const std::shared_ptr<const CPVRChannel> channel{groupMember->Channel()};

        if (byClientAndProviderFilter.Filter(channel))
          continue;

        if (showHiddenChannels != channel->IsHidden())
          continue;

        if (playedOnly && !channel->LastWatched())
          continue;

        if (dateAdded)
        {
          const CDateTime dtChannelAdded{channel->DateTimeAdded()};
          if (!dtChannelAdded.IsValid())
            continue;

          const std::shared_ptr<const CPVRClient> client{
              CServiceBroker::GetPVRManager().GetClient(groupMember->ChannelClientID())};
          if (client)
          {
            const CDateTime& dtFirstChannelsAdded{client->GetDateTimeFirstChannelsAdded()};
            if (dtFirstChannelsAdded.IsValid() && (dtChannelAdded <= dtFirstChannelsAdded))
            {
              continue; // Ignore channels added on very first GetChannels call for the client.
            }
          }
        }

        auto item{std::make_shared<CFileItem>(groupMember)};
        if (dateAdded)
          item->SetProperty("hideable", true);

        results.Add(std::move(item));
      }
      return true;
    }
  }
  return false;
}

namespace
{

bool GetTimersRootDirectory(const CPVRTimersPath& path,
                            bool bHideDisabled,
                            const std::vector<std::shared_ptr<CPVRTimerInfoTag>>& timers,
                            CFileItemList& results)
{
  bool bRadio = path.IsRadio();
  bool bRules = path.IsRules();

  for (const auto& timer : timers)
  {
    if ((bRadio == timer->IsRadio() ||
         (bRules && timer->ClientChannelUID() == PVR_TIMER_ANY_CHANNEL)) &&
        (bRules == timer->IsTimerRule()) && (!bHideDisabled || !timer->IsDisabled()))
    {
      const auto item = std::make_shared<CFileItem>(timer);
      const CPVRTimersPath timersPath(path.AsString(), timer->ClientID(), timer->ClientIndex());
      item->SetPath(timersPath.AsString());
      results.Add(item);
    }
  }
  return true;
}

bool GetTimersSubDirectory(const CPVRTimersPath& path,
                           bool bHideDisabled,
                           const std::vector<std::shared_ptr<CPVRTimerInfoTag>>& timers,
                           CFileItemList& results)
{
  bool bRadio = path.IsRadio();
  int iParentId = path.GetParentId();
  int iClientId = path.GetClientId();

  std::shared_ptr<CFileItem> item;

  for (const auto& timer : timers)
  {
    if ((timer->IsRadio() == bRadio) && timer->HasParent() &&
        (iClientId == PVR_CLIENT_INVALID_UID || timer->ClientID() == iClientId) &&
        (timer->ParentClientIndex() == iParentId) && (!bHideDisabled || !timer->IsDisabled()))
    {
      item = std::make_shared<CFileItem>(timer);
      const CPVRTimersPath timersPath(path.AsString(), timer->ClientID(), timer->ClientIndex());
      item->SetPath(timersPath.AsString());
      results.Add(item);
    }
  }
  return true;
}

} // unnamed namespace

bool CPVRGUIDirectory::GetTimersDirectory(CFileItemList& results) const
{
  const CPVRTimersPath path(m_url.GetWithoutOptions());
  if (path.IsValid() && (path.IsTimersRoot() || path.IsTimerRule()))
  {
    bool bHideDisabled = false;
    if (m_url.HasOption("view"))
    {
      const std::string view = m_url.GetOption("view");
      if (view == "hidedisabled")
      {
        bHideDisabled = true;
      }
      else
      {
        CLog::LogF(LOGERROR, "Unsupported value '{}' for url parameter 'view'", view);
        return false;
      }
    }
    else
    {
      bHideDisabled = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          CSettings::SETTING_PVRTIMERS_HIDEDISABLEDTIMERS);
    }

    const std::vector<std::shared_ptr<CPVRTimerInfoTag>> timers =
        CServiceBroker::GetPVRManager().Timers()->GetAll();

    if (path.IsTimersRoot())
    {
      /* Root folder containing either timer rules or timers. */
      return GetTimersRootDirectory(path, bHideDisabled, timers, results);
    }
    else if (path.IsTimerRule())
    {
      /* Sub folder containing the timers scheduled by the given timer rule. */
      return GetTimersSubDirectory(path, bHideDisabled, timers, results);
    }
  }

  return false;
}

bool CPVRGUIDirectory::GetProvidersDirectory(CFileItemList& results) const
{
  const CPVRProvidersPath path(m_url.GetWithoutOptions());
  if (path.IsValid())
  {
    if (path.IsProvidersRoot())
    {
      const std::shared_ptr<const CPVRChannelGroupsContainer> groups{
          CServiceBroker::GetPVRManager().ChannelGroups()};
      const std::shared_ptr<const CPVRRecordings> recordings{
          CServiceBroker::GetPVRManager().Recordings()};
      const std::vector<std::shared_ptr<CPVRProvider>> providers{
          CServiceBroker::GetPVRManager().Providers()->GetProviders()};
      for (const auto& provider : providers)
      {
        if (!groups->HasChannelForProvider(path.IsRadio(), provider->GetClientId(),
                                           provider->GetUniqueId()) &&
            !recordings->HasRecordingForProvider(path.IsRadio(), provider->GetClientId(),
                                                 provider->GetUniqueId()))
          continue;

        const CPVRProvidersPath providerPath{path.GetKind(), provider->GetClientId(),
                                             provider->GetUniqueId()};
        results.Add(std::make_shared<CFileItem>(providerPath.AsString(), provider));
      }
      return true;
    }
    else if (path.IsProvider())
    {
      // Add items for channels and recordings, if at least one matching is available.

      const std::shared_ptr<const CPVRChannelGroupsContainer> groups{
          CServiceBroker::GetPVRManager().ChannelGroups()};
      const unsigned int channelCount{groups->GetChannelCountByProvider(
          path.IsRadio(), path.GetClientId(), path.GetProviderUid())};
      if (channelCount > 0)
      {
        const CPVRProvidersPath channelsPath{path.GetKind(), path.GetClientId(),
                                             path.GetProviderUid(), CPVRProvidersPath::CHANNELS};
        auto channelsItem{std::make_shared<CFileItem>(channelsPath.AsString(), true)};
        channelsItem->SetLabel(g_localizeStrings.Get(19019)); // Channels
        channelsItem->SetArt("icon", "DefaultPVRChannels.png");
        channelsItem->SetProperty("totalcount", channelCount);
        results.Add(std::move(channelsItem));
      }

      const std::shared_ptr<const CPVRRecordings> recordings{
          CServiceBroker::GetPVRManager().Recordings()};
      const unsigned int recordingCount{recordings->GetRecordingCountByProvider(
          path.IsRadio(), path.GetClientId(), path.GetProviderUid())};
      if (recordingCount > 0)
      {
        const CPVRProvidersPath recordingsPath{path.GetKind(), path.GetClientId(),
                                               path.GetProviderUid(),
                                               CPVRProvidersPath::RECORDINGS};
        auto recordingsItem{std::make_shared<CFileItem>(recordingsPath.AsString(), true)};
        recordingsItem->SetLabel(g_localizeStrings.Get(19017)); // Recordings
        recordingsItem->SetArt("icon", "DefaultPVRRecordings.png");
        recordingsItem->SetProperty("totalcount", recordingCount);
        results.Add(std::move(recordingsItem));
      }

      return true;
    }
    else if (path.IsChannels())
    {
      // Add all channels served by this provider.
      const std::shared_ptr<const CPVRChannelGroup> group{
          CServiceBroker::GetPVRManager().ChannelGroups()->GetGroupAll(path.IsRadio())};
      if (group)
      {
        const bool checkUid{path.GetProviderUid() != PVR_PROVIDER_INVALID_UID};
        const std::vector<std::shared_ptr<CPVRChannelGroupMember>> allGroupMembers{
            group->GetMembers(CPVRChannelGroup::Include::ONLY_VISIBLE)};
        for (const auto& allGroupMember : allGroupMembers)
        {
          const std::shared_ptr<const CPVRChannel> channel{allGroupMember->Channel()};

          if (channel->ClientID() != path.GetClientId())
            continue;

          if (checkUid && channel->ClientProviderUid() != path.GetProviderUid())
            continue;

          results.Add(std::make_shared<CFileItem>(allGroupMember));
        }
        return true;
      }
    }
    else if (path.IsRecordings())
    {
      // Add all recordings served by this provider.
      const bool checkUid{path.GetProviderUid() != PVR_PROVIDER_INVALID_UID};
      const std::vector<std::shared_ptr<CPVRRecording>> recordings{
          CServiceBroker::GetPVRManager().Recordings()->GetAll()};
      for (const auto& recording : recordings)
      {
        if (recording->IsRadio() != path.IsRadio())
          continue;

        if (recording->ClientID() != path.GetClientId())
          continue;

        if (checkUid && recording->ClientProviderUid() != path.GetProviderUid())
          continue;

        results.Add(std::make_shared<CFileItem>(recording));
      }
      return true;
    }
  }
  return false;
}
