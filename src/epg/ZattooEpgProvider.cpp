#include "ZattooEpgProvider.h"
#include "rapidjson/document.h"
#include "../client.h"
#include "../Utils.h"
#include <ctime>


using namespace rapidjson;

ZattooEpgProvider::ZattooEpgProvider(
    kodi::addon::CInstancePVRClient *addon,
    std::string providerUrl,
    EpgDB &epgDB,
    HttpClient &httpClient,
    Categories &categories,
    std::map<int, ZatChannel> &channelsByUid,
    std::string powerHash
  ):
  EpgProvider(addon),
  m_epgDB(epgDB),
  m_httpClient(httpClient),
  m_categories(categories),
  m_powerHash(powerHash),
  m_providerUrl(providerUrl),
  m_channelsByUid(channelsByUid)
{
  time(&lastCleanup);
}

ZattooEpgProvider::~ZattooEpgProvider() { }

bool ZattooEpgProvider::LoadEPGForChannel(ZatChannel &notUsed, time_t iStart, time_t iEnd) {
  time_t tempStart = iStart - (iStart % (3600 / 2)) - 86400;
  tempStart = SkipAlreadyLoaded(tempStart, iEnd);
  time_t tempEnd = tempStart + 3600 * 5; //Add 5 hours
  CleanupAlreadyLoaded();
  while (tempStart < iEnd)
  {
    if (tempEnd > iEnd) {
      tempEnd = iEnd;
    }
    std::ostringstream urlStream;
    urlStream << m_providerUrl << "/zapi/v3/cached/" + m_powerHash + "/guide"
        << "?end=" << tempEnd << "&start=" << tempStart
        << "&format=json";

    int statusCode;
    std::string jsonString = m_httpClient.HttpGetCached(urlStream.str(), 86400, statusCode);

    Document doc;
    doc.Parse(jsonString.c_str());
    if (doc.GetParseError())
    {
      kodi::Log(ADDON_LOG_ERROR, "Loading epg faild from %lu to %lu", iStart, iEnd);
      return false;
    }
    RegisterAlreadyLoaded(tempStart, tempEnd);
    const Value& channels = doc["channels"];
    
    std::lock_guard<std::mutex> lock(sendEpgToKodiMutex);
    m_epgDB.BeginTransaction();
    for (Value::ConstMemberIterator iter = channels.MemberBegin(); iter != channels.MemberEnd(); ++iter) {
      std::string cid = iter->name.GetString();

      int uniqueChannelId = Utils::GetChannelId(cid.c_str());
      
      if (m_channelsByUid.count(uniqueChannelId) == 0) {
        continue;
      }

      const Value& programs = iter->value;
      for (Value::ConstValueIterator itr1 = programs.Begin();
          itr1 != programs.End(); ++itr1)
      {
        const Value& program = (*itr1);

        const Type& checkType = program["t"].GetType();
        if (checkType != kStringType)
          continue;
        
        kodi::addon::PVREPGTag tag;
        tag.SetUniqueBroadcastId(static_cast<unsigned int>(program["id"].GetInt()));
        tag.SetTitle(Utils::JsonStringOrEmpty(program, "t"));
        tag.SetUniqueChannelId(static_cast<unsigned int>(uniqueChannelId));
        tag.SetStartTime(program["s"].GetInt());
        tag.SetEndTime(program["e"].GetInt());
        tag.SetPlotOutline(Utils::JsonStringOrEmpty(program, "et"));
        tag.SetPlot(Utils::JsonStringOrEmpty(program, "et"));
        tag.SetOriginalTitle(""); /* not supported */
        tag.SetCast(""); /* not supported */
        tag.SetDirector(""); /*SA not supported */
        tag.SetWriter(""); /* not supported */
        tag.SetYear(0); /* not supported */
        tag.SetIMDBNumber(""); /* not supported */
        tag.SetIconPath(Utils::GetImageUrl(Utils::JsonStringOrEmpty(program, "i_t")));
        tag.SetParentalRating(0); /* not supported */
        tag.SetStarRating(0); /* not supported */
        tag.SetSeriesNumber(EPG_TAG_INVALID_SERIES_EPISODE); /* not supported */
        tag.SetEpisodeNumber(EPG_TAG_INVALID_SERIES_EPISODE); /* not supported */
        tag.SetEpisodePartNumber(EPG_TAG_INVALID_SERIES_EPISODE); /* not supported */
        tag.SetEpisodeName(""); /* not supported */
               
        const Value& genres = program["g"];
        std::string genreString;
        for (Value::ConstValueIterator itr2 = genres.Begin();
            itr2 != genres.End(); ++itr2)
        {
          genreString = (*itr2).GetString();
          break;
        }

        int genre = m_categories.Category(genreString);
        if (genre)
        {
          tag.SetGenreSubType(genre & 0x0F);
          tag.SetGenreType(genre & 0xF0);
        }
        else
        {
          tag.SetGenreType(EPG_GENRE_USE_STRING);
          tag.SetGenreSubType(0); /* not supported */
          tag.SetGenreDescription(genreString);
        }
        
        EpgDBInfo epgDBInfo;
        epgDBInfo.programId = program["id"].GetInt();
        epgDBInfo.recordUntil = Utils::JsonIntOrZero(program, "rg_u");
        epgDBInfo.replayUntil = Utils::JsonIntOrZero(program, "sr_u");
        epgDBInfo.restartUntil = Utils::JsonIntOrZero(program, "ry_u");
        m_epgDB.Insert(epgDBInfo);
        SendEpg(tag);
      }
      m_epgDB.EndTransaction();
    }
    tempStart = SkipAlreadyLoaded(tempEnd, iEnd);
    tempEnd = tempStart + 3600 * 5; //Add 5 hours
  }
  return true;
}

void ZattooEpgProvider::CleanupAlreadyLoaded() {
  time_t now;
  time(&now);
  if (lastCleanup < now + 60) {
    return;
  }
  lastCleanup = now;
  
  m_loadedTimeslots.erase(
      std::remove_if(m_loadedTimeslots.begin(), m_loadedTimeslots.end(),
          [&now](const LoadedTimeslots & o) { return o.loaded < now - 180; }),
          m_loadedTimeslots.end());
}

void ZattooEpgProvider::RegisterAlreadyLoaded(time_t startTime, time_t endTime) {
  LoadedTimeslots slot;
  slot.start = startTime;
  slot.end = endTime;
  time(&slot.loaded);
  m_loadedTimeslots.push_back(slot);
}

time_t ZattooEpgProvider::SkipAlreadyLoaded(time_t startTime, time_t endTime) {
  time_t newStartTime = startTime;
  std::vector<LoadedTimeslots> slots(m_loadedTimeslots.begin(), m_loadedTimeslots.end());
  for (LoadedTimeslots slot: slots) {
    if (slot.start <= newStartTime && slot.end > newStartTime) {
      newStartTime = slot.end;
      if (newStartTime > endTime) {
        break;
      }
    }
  }
  return newStartTime;
}

