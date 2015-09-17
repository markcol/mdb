#include "storage.h"
#include "page_manager.h"
#include "utils/exception.h"

#include <ctime>
#include <cmath>
#include <sstream>
#include <iterator>

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

using namespace storage;


Storage::Storage()
    : m_cache_pool(defaultcachePoolSize, defaultcacheSize) {
  m_cache = m_cache_pool.getCache();
  m_cache->setStorage(this);
  m_cache_writer.setStorage(this);
  m_cache_writer.start();
  m_past_time = 0;
  m_closed = false;
}

Storage::~Storage() { 
	if (!m_closed) { 
		this->Close(); 
    }
}

void Storage::Close() {
  if (!m_cache_writer.stoped()) {
    this->writeCache();
    m_cache_writer.stop();
  }

  PageManager::get()->closeCurrentPage();
  PageManager::stop();
  m_closed = true;
}

Storage::Storage_ptr Storage::Create(const std::string &ds_path,
                                              uint64_t page_size) {
  Storage::Storage_ptr result(new Storage);
  

  if (fs::exists(ds_path)) {
    if (!utils::rm(ds_path)) {
      throw utils::Exception::CreateAndLog(
          POSITION, "can`t create. remove error: " + ds_path);
    }
  }

  fs::create_directory(ds_path);
  result->m_path = std::string(ds_path);
  PageManager::start(result->m_path);
  PageManager::get()->default_page_size = page_size;
  PageManager::get()->createNewPage();
  return result;
}

Storage::Storage_ptr Storage::Open(const std::string &ds_path) {

  Storage::Storage_ptr result(new Storage);

  if (!fs::exists(ds_path)) {
    throw utils::Exception::CreateAndLog(POSITION, ds_path + " not exists.");
  }

  auto pages = utils::ls(ds_path, ".page");
  result->m_path = std::string(ds_path);

  PageManager::start(result->m_path);
  std::string maxTimePage = PageManager::get()->getOldesPage();
  PageManager::get()->open(maxTimePage);

  return result;
}

bool Storage::havePage2Write() const {
    return (PageManager::get()->getCurPage() != nullptr) && (!PageManager::get()->getCurPage()->isFull());
}

append_result Storage::append(const Meas& m) {
  std::lock_guard<std::mutex> guard(m_write_mutex);
  append_result res{};
  while (res.writed == 0) {
    res = m_cache->append(m, m_past_time);
    if (res.writed == 0) {
      this->writeCache();
    }
	if (res.ignored != 0) {
		logger_info("DataStorage: ignored on write:" << res.ignored);
	}
  }
  return res;
}

append_result Storage::append(const Meas::PMeas begin, const size_t meas_count) {
  std::lock_guard<std::mutex> guard(m_write_mutex);
  if (m_cache->isFull()) {
    this->writeCache();
  }
  size_t to_write = meas_count;
  append_result result{};
  while (to_write > 0) {
    auto wrt_res =
        m_cache->append(begin + (meas_count - to_write), to_write, m_past_time);

    if (wrt_res.writed != to_write) {
      this->writeCache();
    }
    to_write -= wrt_res.writed;
    result = result + wrt_res;
  }
  if (result.ignored != 0) {
	  logger_info("DataStorage: ignored on write:" << result.ignored);
  }
  return result;
}

void Storage::writeCache() {
  if (m_cache->size() == 0) {
    return;
  }
  m_cache->sync_begin();

  m_cache_writer.add(m_cache);

  // FIX must use more smart method.
  m_cache = nullptr;
  while (true) {
    m_cache = m_cache_pool.getCache();
    if (m_cache != nullptr) {
      break;
    }
  }
  m_cache->setStorage(this);
}

PStorageReader Storage::readInterval(Time from, Time to) {
	static IdArray empty{};
	return this->readInterval(empty, 0, 0, from, to);
}

PStorageReader Storage::readInterval(const IdArray &ids,
                                     storage::Flag source, storage::Flag flag,
                                     Time from, Time to) {
	std::lock_guard<std::mutex> guard(m_write_mutex);
    this->writeCache();
    while (true) {
        if (!m_cache_writer.isBusy()) {
            break;
        }
    }

    auto sr=new StorageReader();
    PStorageReader result(sr);

    this->m_cache_writer.pause_work();

    auto pages = PageManager::get()->pagesByTime();
	std::reverse(std::begin(pages), std::end(pages));

	std::list<std::string> pages_to_read{};

	
    for (size_t i=0;i<pages.size();i++) {
		auto pinfo = pages[i];
		auto page_name = pinfo.name;
		auto hdr = pinfo.header;
		
        // [min from to max]
        if ((hdr.minTime <= from) && (hdr.maxTime >= to)) {
				pages_to_read.push_back(page_name);
				continue;
		}

        // [...max] from [min...]
		if (hdr.minTime > from) {
			if ((i > 0) && (pages[i - 1].header.maxTime <= from)) {
				pages_to_read.push_back(pages[i - 1].name);
				pages_to_read.push_back(page_name);
				continue;
			}
		}

        // [min from max] to
		if ((hdr.minTime >= from) && (hdr.maxTime <= to)) {
			pages_to_read.push_back(page_name);
			continue;
		}
		
        // min from  max
        // to max
        if ((from >= hdr.minTime) && (hdr.maxTime >= from)){
			pages_to_read.push_back(page_name);
            if(i>0){
				result->prev_interval_page=pages[i-1].name;
            }
			continue;
		}

        if (to <= hdr.maxTime) {
            pages_to_read.push_back(page_name);
            continue;
        }

    }
	
    result->ids=ids;
    result->from=from;
    result->to=to;
    result->source=source;
    result->flag=flag;
	for (auto page_name : pages_to_read) {
        result->addPage(page_name);
	}

    this->m_cache_writer.continue_work();

    return result;
}


IdArray Storage::loadCurValues(const IdArray&ids) {
	auto from = *std::min_element(ids.begin(),ids.end());
	auto to = *std::max_element(ids.begin(), ids.end());
	std::vector<PageManager::PageInfo> pages_vector = PageManager::get()->pagesByTime();
	std::vector<PageManager::PageInfo> page_time_vector{};
	
	for (auto p : pages_vector) {
		if (HeaderIdIntervalCheck(from, to, p.header)) {
			page_time_vector.push_back(p);
		}
	}

	IdSet id_set(ids.begin(), ids.end());

	storage::Page::Page_ptr page2read = storage::Page::Open(page_time_vector.front().name, true);
	Page::WriteWindow ww = page2read->getWriteWindow();
	for (auto m : ww) {
		m_cur_values.writeValue(m);
		id_set.erase(m.id);
	}
	page2read->readComplete();
    if(id_set.size()!=0){
        for(auto id:id_set){
            logger("DataStorage::loadCurValues: not found  "<<id);
        }
    }
    IdArray result(id_set.begin(),id_set.end());
    return result;
}


Time Storage::pastTime() const { return m_past_time; }

void Storage::setPastTime(const Time &t) { m_past_time = t; }

void Storage::enableCacheDynamicSize(bool flg) {
  m_cache_pool.enableDynamicSize(flg);
}

bool Storage::cacheDynamicSize() const {
  return m_cache_pool.dynamicSize();
}

size_t Storage::getPoolSize()const{
    return m_cache_pool.getPoolSize();
}

void Storage::setPoolSize(size_t sz){
    m_cache_pool.setPoolSize(sz);
}

size_t Storage::getCacheSize()const{
    return m_cache_pool.getCacheSize();
}

void Storage::setCacheSize(size_t sz){
    m_cache_pool.setCacheSize(sz);
}


Meas::MeasList Storage::curValues(const IdArray&ids) {
	this->writeCache();
	while (true) {
		if (!m_cache_writer.isBusy()) {
			break;
		}
	}
	return m_cur_values.readValue(ids);
}

StorageReader::StorageReader():
    m_pages(),
	m_current_reader(nullptr), 
	prev_interval_page(""){

}

bool StorageReader::isEnd(){
    if(this->m_pages.size()==0){
        return m_current_reader==nullptr?true:m_current_reader->isEnd();
    }else{
        return false;
    }
}

void StorageReader::readNext(Meas::MeasList*output){
	assert(output != nullptr);
    if(isEnd()){
        return;
    }

    if(m_current_reader==nullptr){
        auto page_name=m_pages.front();
        m_pages.pop_front();
        storage::Page::Page_ptr page2read = storage::Page::Open(page_name, true);
        m_current_reader = page2read->readInterval(ids, source, flag, from, to);
    }

	if ((m_current_reader->m_page->getHeader().minTime <= this->from) && (prev_interval_page!="")) {
		Meas::MeasList sub_result;
		
		m_current_reader->readNext(&sub_result);
		std::copy(sub_result.begin(), sub_result.end(), std::back_inserter(*output));

		std::copy(sub_result.begin(), sub_result.end(), std::back_inserter(localResult));
	} else {
		m_current_reader->readNext(output);
	}

    if(m_current_reader->isEnd()){
		if ((m_current_reader->m_page->getHeader().minTime <= this->from) && (prev_interval_page != "")) {
			/// read data not in interval if needed
			this->readNotIntervalData(output);
			localResult.clear();
		}
        m_current_reader=nullptr;
    }
	
}

void StorageReader::addPage(std::string page_name){
    this->m_pages.push_back(page_name);
}

void StorageReader::readNotIntervalData(Meas::MeasList*output) {
	/// find meas not in founded interval [from,to]
	std::set<Id> not_found{ids.begin(),ids.end()};
	Time max_time=0;
	bool max_time_set = false;
	for (auto m : localResult) {
		if (m.time > from) {
			not_found.insert(m.id);
			if (max_time_set) {
				max_time = std::max(max_time, m.time);
			} else {
				max_time = m.time;
				max_time_set = true;
			}
		} else {
			not_found.erase(m.id);
		}
	}

	if (not_found.size() == 0) {
		return;
	}

	localResult.clear();

	/// search not founded values in [page.minTime, min founded time]
	IdArray new_ids{ not_found.begin(), not_found.end() };
	auto page_dup = PageManager::get()->open(m_current_reader->m_page->fileName(), true);
	auto new_reader = page_dup->readInterval(new_ids, source, flag, m_current_reader->m_page->getHeader().minTime, max_time - 1);

	while (!new_reader->isEnd()) {
		new_reader->readNext(&localResult);
	}
	new_reader = nullptr;

	/// put to output(lol) and erase from not_found set.
	for (auto it = localResult.rbegin(); it != localResult.rend(); it++) {
		auto search_res = not_found.find(it->id);
		if ((search_res != not_found.end()) && (it->time <= from)) {
			not_found.erase(it->id);
			output->push_front(*it);
		}
		if (not_found.size() == 0) {
			break;
		}
	}

	/// search in write window of prev. by time page.
	/// [ prevMax] [curmin]
	if ((localResult.size() == 0) || (not_found.size() != 0)) {
		auto prev_page = PageManager::get()->open(this->prev_interval_page, true);
		auto ww = prev_page->getWriteWindow();
		prev_page->readComplete();

		for (auto nf_id : not_found) {
			for (auto value : ww) {
				if (value.id == nf_id) {
					if (m_current_reader->checkValueFlags(value)) {
						output->push_front(value);
					}
					break;
				}
			}
		}
	}
}