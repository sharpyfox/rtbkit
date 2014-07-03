/** exchange_source.cc                                 -*- C++ -*-
    Eric Robert, 6 May 2013
    Copyright (c) 2013 Datacratic.  All rights reserved.

    Implementation of exchange sources

*/

#include <mutex>

#include "jml/arch/spinlock.h"
#include "jml/arch/timers.h"

#include "exchange_source.h"
#include "soa/service/http_header.h"

#include <dlfcn.h>
#include <array>

using namespace RTBKIT;

ExchangeSource::
ExchangeSource(NetworkAddress address_) :
    address(std::move(address_)),
    addr(0),
    fd(-1)
{
    auto seed = reinterpret_cast<size_t>(this);
    rng.seed((uint32_t) seed);

    addrinfo hint = { 0, AF_INET, SOCK_STREAM, 0, 0, 0, 0, 0 };

    char const * host = 0;
    if(address.host != "localhost") host = address.host.c_str();
    int res = getaddrinfo(host, std::to_string(address.port).c_str(), &hint, &addr);
    ExcCheckErrno(!res, "getaddrinfo failed");
    if(!addr) throw ML::Exception("cannot find suitable address");

    connect();
    std::cerr << "sending to " << address.host << ":" << address.port << std::endl;
}


ExchangeSource::
~ExchangeSource()
{
    if (addr) freeaddrinfo(addr);
    if (fd >= 0) close(fd);
}


void
ExchangeSource::
connect()
{
    if(fd >= 0) close(fd);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    ExcCheckErrno(fd != -1, "socket failed");

    for(;;) {
        int res = ::connect(fd, addr->ai_addr, addr->ai_addrlen);
        if(res == 0) {
            break;
        }

        ML::sleep(0.1);
    }
}


std::string
ExchangeSource::
read()
{
    std::array<char, 16384> buffer;

    int res = recv(fd, buffer.data(), buffer.size(), 0);
    if (res == 0 || (res == -1 && errno == ECONNRESET)) {
        return "";
    }

    ExcCheckErrno(res != -1, "recv");
    return std::string(buffer.data(), res);
}


void
ExchangeSource::
write(const std::string & data)
{
    const char * current = data.c_str();
    const char * end = current + data.size();

    while (current != end) {
        int res = send(fd, current, end - current, MSG_NOSIGNAL);
        if(res == 0 || res == -1) {
            connect();
            current = data.c_str();
            continue;
        }

        current += res;
    }

    ExcAssertEqual((void *)current, (void *)end);
}



BidSource::BidSource(NetworkAddress address) :
    ExchangeSource(std::move(address)),
    bidForever(true),
    bidCount(0),
    bidLifetime(0),
    key(rng.random()) {
}


BidSource::BidSource(NetworkAddress address, int lifetime) :
    ExchangeSource(std::move(address)),
    bidForever(false),
    bidCount(0),
    bidLifetime(lifetime),
    key(rng.random()) {
}


BidSource::
BidSource(Json::Value const & json) :
    ExchangeSource(json["url"].asString()),
    bidForever(true),
    bidCount(0),
    bidLifetime(0),
    key(rng.random()) {
    if(json.isMember("lifetime")) {
        bidForever = false;
        bidLifetime = json["lifetime"].asInt();
    }
}


bool
BidSource::
isDone() const {
    return bidForever ? false : bidLifetime <= bidCount;
}


BidRequest
BidSource::
sendBidRequest() {
    ++bidCount;
    return generateRandomBidRequest();
}


std::pair<bool, std::vector<ExchangeSource::Bid>>
BidSource::
receiveBid() {
    return parseResponse(read());
}


WinSource::
WinSource(NetworkAddress address) :
    ExchangeSource(std::move(address)) {
}


WinSource::
WinSource(Json::Value const & json) :
    ExchangeSource(json["url"].asString()) {
}

void
WinSource::
sendWin(const BidRequest& bidRequest, const Bid& bid, const Amount& winPrice)
{
}



EventSource::
EventSource(NetworkAddress address) :
    ExchangeSource(std::move(address)) {
}


EventSource::
EventSource(Json::Value const & json) :
    ExchangeSource(json["url"].asString()) {
}


void
EventSource::
sendImpression(const BidRequest& bidRequest, const Bid& bid)
{
}


void
EventSource::
sendClick(const BidRequest& bidRequest, const Bid& bid)
{
}

namespace {
    typedef std::lock_guard<ML::Spinlock> Guard;
    static ML::Spinlock bidLock;
    static ML::Spinlock winLock;
    static ML::Spinlock eventLock;
    static std::unordered_map<std::string, BidSource::Factory> bidFactories;
    static std::unordered_map<std::string, WinSource::Factory> winFactories;
    static std::unordered_map<std::string, EventSource::Factory> eventFactories;
}


BidSource::Factory getBidFactory(std::string const & name) {
    // see if it's already existing
    {
        Guard guard(bidLock);
        auto i = bidFactories.find(name);
        if (i != bidFactories.end()) return i->second;
    }

    // else, try to load the exchange library
    std::string path = "lib" + name + "_bid_request.so";
    void * handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        throw ML::Exception("couldn't find bid request/source library " + path);
    }

    // if it went well, it should be registered now
    Guard guard(bidLock);
    auto i = bidFactories.find(name);
    if (i != bidFactories.end()) return i->second;

    throw ML::Exception("couldn't find bid source name " + name);
}


void BidSource::registerBidSourceFactory(std::string const & name, Factory callback) {
    Guard guard(bidLock);
    if (!bidFactories.insert(std::make_pair(name, callback)).second)
        throw ML::Exception("already had a bid source factory registered");
}


std::unique_ptr<BidSource> BidSource::createBidSource(Json::Value const & json) {
    auto name = json.get("type", "unknown").asString();
    auto factory = getBidFactory(name);
    return std::unique_ptr<BidSource>(factory(json));
}


WinSource::Factory getWinFactory(std::string const & name) {
    // see if it's already existing
    {
        Guard guard(winLock);
        auto i = winFactories.find(name);
        if (i != winFactories.end()) return i->second;
    }

    // else, try to load the adserver library
    std::string path = "lib" + name + "_adserver.so";
    void * handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        throw ML::Exception("couldn't find adserver library " + path);
    }

    // if it went well, it should be registered now
    Guard guard(winLock);
    auto i = winFactories.find(name);
    if (i != winFactories.end()) return i->second;

    throw ML::Exception("couldn't find win source name " + name);
}


void WinSource::registerWinSourceFactory(std::string const & name, Factory callback) {
    Guard guard(winLock);
    if (!winFactories.insert(std::make_pair(name, callback)).second)
        throw ML::Exception("already had a win source factory registered");
}


std::unique_ptr<WinSource> WinSource::createWinSource(Json::Value const & json) {
    auto name = json.get("type", "unknown").asString();
    if(name == "none") {
        return 0;
    }

    auto factory = getWinFactory(name);
    return std::unique_ptr<WinSource>(factory(json));
}



EventSource::Factory getEventFactory(std::string const & name) {
    // see if it's already existing
    {
        Guard guard(eventLock);
        auto i = eventFactories.find(name);
        if (i != eventFactories.end()) return i->second;
    }

    // else, try to load the adserver library
    std::string path = "lib" + name + "_adserver.so";
    void * handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        throw ML::Exception("couldn't find adserver library " + path);
    }

    // if it went well, it should be registered now
    Guard guard(eventLock);
    auto i = eventFactories.find(name);
    if (i != eventFactories.end()) return i->second;

    throw ML::Exception("couldn't find event source name " + name);
}


void EventSource::registerEventSourceFactory(std::string const & name, Factory callback) {
    Guard guard(eventLock);
    if (!eventFactories.insert(std::make_pair(name, callback)).second)
        throw ML::Exception("already had a event source factory registered");
}


std::unique_ptr<EventSource> EventSource::createEventSource(Json::Value const & json) {
    auto name = json.get("type", "unknown").asString();
    if(name == "none") {
        return 0;
    }

    auto factory = getEventFactory(name);
    return std::unique_ptr<EventSource>(factory(json));
}

