#include "storage.h"
#include <boost/filesystem.hpp>
#include <utils/Exception.h>
#include <ctime>
#include <cmath>
#include <sstream>
#include <iterator>

using namespace storage;
namespace fs=boost::filesystem;

const size_t defaultcacheSize = 10000;
const size_t defaultcachePoolSize=100;

fs::path getOldesPage(const std::list<fs::path> &pages){
    if(pages.size()==1)
        return pages.front();
    fs::path maxTimePage;
    Time maxTime=0;
    for(auto p:pages){
        storage::Page::Header hdr=storage::Page::ReadHeader(p.string());
        Time cur_time=hdr.maxTime;
        if(maxTime<cur_time || cur_time==0){
            maxTime=cur_time;
            maxTimePage=p;
        }
    }
    if(maxTimePage.string().size()==0){
        throw utils::Exception::CreateAndLog(POSITION, "open error. page not found.");
    }
    return maxTimePage;
}

std::string getNewPageUniqueName(const std::string &ds_path){
    fs::path page_path;
    uint32_t suffix=0;

    while(true){
        if(page_path.string().length()!=0 && !fs::exists(page_path))
            break;

        page_path.clear();

        std::stringstream ss;
        ss<<std::time(nullptr)<<'_'<<suffix;
        ++suffix;
        page_path.append(ds_path);
        page_path.append(ss.str()+".page");
    }

    return page_path.string();
}

DataStorage::DataStorage() :m_cache_pool(defaultcachePoolSize,defaultcacheSize) {
    m_cache=m_cache_pool.getCache();
    m_cache_writer.setStorage(this);
    m_cache_writer.start();
	m_past_time = 0;
}

DataStorage::~DataStorage(){
	this->Close();
}

void DataStorage::Close() {

    if (!m_cache_writer.stoped()){
        this->writeCache();
        m_cache_writer.stop();
    }

    if (m_curpage != nullptr) {
        m_curpage->close();
        m_curpage = nullptr;
	}

}

Page::PPage DataStorage::getCurPage(){
    return m_curpage;
}

DataStorage::PDataStorage DataStorage::Create(const std::string& ds_path, uint64_t page_size){
    DataStorage::PDataStorage result(new DataStorage);
    result->m_default_page_size=page_size;

    if(fs::exists(ds_path)){
        if(!utils::rm(ds_path)){
            throw utils::Exception::CreateAndLog(POSITION, "can`t create. remove error: "+ds_path);
        }
    }

    fs::create_directory(ds_path);
    result->m_path=std::string(ds_path);
    result->createNewPage();
    return result;
}

DataStorage::PDataStorage DataStorage::Open(const std::string& ds_path){
    DataStorage::PDataStorage result(new DataStorage);

    if(!fs::exists(ds_path)){
        throw utils::Exception::CreateAndLog(POSITION, ds_path+" not exists.");
    }

    auto pages=utils::ls(ds_path,".page");
    fs::path maxTimePage=getOldesPage(pages);


    result->m_curpage=storage::Page::Open(maxTimePage.string());
    result->m_path=std::string(ds_path);
    return result;
}

void DataStorage::createNewPage(){
    if(m_curpage!=nullptr){
        m_curpage->close();
        m_curpage=nullptr;
    }

    std::string page_path=getNewPageUniqueName(m_path);

    m_curpage=Page::Create(page_path, this->m_default_page_size);
}

bool DataStorage::havePage2Write()const{
    return this->m_curpage!=nullptr && !this->m_curpage->isFull();
}

append_result DataStorage::append(const Meas::PMeas m) {
    std::lock_guard<std::mutex> guard(m_write_mutex);
	append_result res{};
	while (res.writed==0) {
		res = m_cache->append(*m, m_past_time);
		if (res.writed==0) {
			this->writeCache();
		}
	}
	return res;
}

append_result DataStorage::append(const Meas::PMeas begin, const size_t meas_count) {
	std::lock_guard<std::mutex> guard(m_write_mutex);
    if (m_cache->isFull()) {
		this->writeCache();
	}
	size_t to_write = meas_count;
	append_result result{};
	while (to_write > 0) {
		auto wrt_res = m_cache->append(begin + (meas_count - to_write), to_write, m_past_time);
		
		if (wrt_res.writed != to_write) {
			this->writeCache();
		}
		to_write -= wrt_res.writed;
		result =result+ wrt_res;
	}
	return result;
}

void DataStorage::writeCache() {
    if (m_cache->size() == 0) {
		return;
	}
    m_cache->sync_begin();

    m_cache_writer.add(m_cache);

    // FIX must use more smart method.
    m_cache=nullptr;
    while(true){
        m_cache=m_cache_pool.getCache();
        if(m_cache!=nullptr){
            break;
        }
    }
    /*m_cache->asArray(m_cache_output);
    size_t meas_count = m_cache->size();
    size_t to_write = m_cache->size();

	while (to_write > 0) {
		size_t writed = m_curpage->append(m_cache_output + (meas_count - to_write), to_write);
		if (writed != to_write) {
			this->createNewPage();
		}
		to_write -= writed;
	}
    m_cache->clear();*/
}

bool HeaderIntervalCheck(Time from, Time to, Page::Header hdr) {
	if (hdr.minTime >= from || hdr.maxTime <= to){
		return true;
	} else {
		return false;
	}
}

Meas::MeasList DataStorage::readInterval(Time from, Time to) {
	return this->readInterval(IdArray{},0,0,from,to);
}

Meas::MeasList DataStorage::readInterval(const IdArray& ids, storage::Flag source, storage::Flag flag, Time from, Time to) {
	this->writeCache();
	while (true) {
		if (!m_cache_writer.isBusy()) {
			break;
		}
	}

	this->m_cache_writer.pause_work();

	Meas::MeasList  list_result;
	auto page_list = pageList();
	for (auto page : page_list) {
		storage::Page::PPage page2read;
		bool shouldClosed = false;
		if (page == m_curpage->fileName()) {
			if (HeaderIntervalCheck(from, to, m_curpage->getHeader())) {
				page2read = m_curpage;
			}
		} else {
			storage::Page::Header hdr = storage::Page::ReadHeader(page);
			if (HeaderIntervalCheck(from, to, hdr)) {
				page2read = storage::Page::Open(page);
				shouldClosed = true;
			} else {
				continue;
			}
		}
		if (page2read == nullptr) {
			continue;
		}

		Meas::MeasList subResult = page2read->readInterval(ids,source,flag,from, to);
		std::copy(subResult.begin(), subResult.end(), std::back_inserter(list_result));
		if (shouldClosed) {
			page2read->close();
		}
	}

	this->m_cache_writer.continue_work();

	if (list_result.size() == 0) {
		return Meas::MeasList{};
	}
	//Meas::MeasArray result{ list_result.begin(), list_result.end() };


	return list_result;
}
std::list<std::string> DataStorage::pageList()const {
	auto page_list = utils::ls(m_path, ".page");
	
	std::list<std::string> result;
	for (auto it = page_list.begin(); it != page_list.end(); ++it) {
		result.push_back(it->string());
	}
	
	return result;
}

Time DataStorage::pastTime()const {
	return m_past_time;
}

void DataStorage::setPastTime(const Time&t) {
	m_past_time = t;
}

void DataStorage::enableCacheDynamicSize(bool flg){
    m_cache_pool.enableDynamicSize(flg);
}

bool DataStorage::cacheDynamicSize()const{
    return m_cache_pool.dynamicSize();
}
