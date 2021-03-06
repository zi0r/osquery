/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <fnmatch.h>

#include <boost/filesystem.hpp>

#include <osquery/logger.h>
#include <osquery/tables.h>

#include "osquery/events/darwin/fsevents.h"

/**
 * @brief FSEvents needs a real/absolute path for watches.
 *
 * When adding a subscription, FSEvents will resolve a depth of recursive
 * symlinks. Increasing the max will make tolerance to odd setups more robust
 * but introduce additional latency during startup.
 */
#define FSEVENTS_MAX_SYMLINK_DEPTH 5

namespace fs = boost::filesystem;

namespace osquery {

std::map<FSEventStreamEventFlags, std::string> kMaskActions = {
    {kFSEventStreamEventFlagItemChangeOwner, "ATTRIBUTES_MODIFIED"},
    {kFSEventStreamEventFlagItemXattrMod, "ATTRIBUTES_MODIFIED"},
    {kFSEventStreamEventFlagItemInodeMetaMod, "ATTRIBUTES_MODIFIED"},
    {kFSEventStreamEventFlagItemCreated, "CREATED"},
    {kFSEventStreamEventFlagItemRemoved, "DELETED"},
    {kFSEventStreamEventFlagItemModified, "UPDATED"},
    {kFSEventStreamEventFlagItemRenamed, "MOVED_TO"},
    {kFSEventStreamEventFlagMustScanSubDirs, "COLLISION_WITHIN"},
    {kFSEventStreamEventFlagUnmount, "UNMOUNTED"},
    {kFSEventStreamEventFlagRootChanged, "ROOT_CHANGED"},
};

REGISTER(FSEventsEventPublisher, "event_publisher", "fsevents");

void FSEventsEventPublisher::restart() {
  if (paths_.empty()) {
    // There are no paths to watch.
    return;
  }

  if (run_loop_ == nullptr) {
    // There is no run loop to restart.
    return;
  }

  // Build paths as CFStrings
  std::vector<CFStringRef> cf_paths;
  for (const auto& path : paths_) {
    auto cf_path =
        CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    cf_paths.push_back(cf_path);
  }

  // The FSEvents watch takes a CFArrayRef
  auto watch_list = CFArrayCreate(nullptr,
                                  reinterpret_cast<const void**>(&cf_paths[0]),
                                  cf_paths.size(),
                                  &kCFTypeArrayCallBacks);

  // Remove any existing stream.
  stop();

  // Create the FSEvent stream
  stream_ = FSEventStreamCreate(nullptr,
                                &FSEventsEventPublisher::Callback,
                                nullptr,
                                watch_list,
                                kFSEventStreamEventIdSinceNow,
                                1,
                                kFSEventStreamCreateFlagFileEvents |
                                    kFSEventStreamCreateFlagNoDefer |
                                    kFSEventStreamCreateFlagWatchRoot);
  if (stream_ != nullptr) {
    // Schedule the stream on the run loop.
    FSEventStreamScheduleWithRunLoop(stream_, run_loop_, kCFRunLoopDefaultMode);
    if (FSEventStreamStart(stream_)) {
      stream_started_ = true;
    } else {
      LOG(ERROR) << "Cannot start FSEvent stream: FSEventStreamStart failed";
    }
  } else {
    LOG(ERROR) << "Cannot create FSEvent stream: FSEventStreamCreate failed";
  }

  // Clean up strings, watch list, and context.
  CFRelease(watch_list);
  for (auto& cf_path : cf_paths) {
    CFRelease(cf_path);
  }
}

void FSEventsEventPublisher::stop() {
  // Stop the stream.
  if (stream_ != nullptr) {
    FSEventStreamStop(stream_);
    stream_started_ = false;
    FSEventStreamUnscheduleFromRunLoop(
        stream_, run_loop_, kCFRunLoopDefaultMode);
    FSEventStreamInvalidate(stream_);
    FSEventStreamRelease(stream_);
    stream_ = nullptr;
  }

  // Stop the run loop.
  if (run_loop_ != nullptr) {
    CFRunLoopStop(run_loop_);
  }
}

void FSEventsEventPublisher::tearDown() {
  stop();

  // Do not keep a reference to the run loop.
  run_loop_ = nullptr;
}

void FSEventsEventPublisher::configure() {
  // Rebuild the watch paths.
  paths_.clear();
  for (auto& subscription : subscriptions_) {
    auto sub = getSubscriptionContext(subscription->context);
    // Check if the requested path was a symlink at configure time.
    boost::system::error_code ec;
    size_t link_depth = 0;
    while (link_depth++ < FSEVENTS_MAX_SYMLINK_DEPTH) {
      // Attempt to follow multiple levels of path links.
      if (fs::is_symlink(sub->path, ec)) {
        if (sub->link_.size() == 0) {
          // Only set the original link path (requested path) once.
          sub->link_ = sub->path;
        }
        auto source_path = fs::read_symlink(sub->path, ec);
        if (!source_path.is_absolute()) {
          source_path = fs::path(sub->link_).parent_path() / source_path;
        }
        sub->path = source_path.string();
      } else {
        break;
      }
    }
    paths_.insert(sub->path);
  }

  // There were no paths in the subscriptions?
  if (paths_.empty()) {
    return;
  }

  restart();
}

Status FSEventsEventPublisher::run() {
  // The run entrypoint executes in a dedicated thread.
  if (run_loop_ == nullptr) {
    run_loop_ = CFRunLoopGetCurrent();
    // Restart the stream creation.
    restart();
  }

  // Start the run loop, it may be removed with a tearDown.
  CFRunLoopRun();
  return Status(0, "OK");
}

void FSEventsEventPublisher::end() { stop(); }

void FSEventsEventPublisher::Callback(
    ConstFSEventStreamRef stream,
    void* callback_info,
    size_t num_events,
    void* event_paths,
    const FSEventStreamEventFlags fsevent_flags[],
    const FSEventStreamEventId fsevent_ids[]) {
  for (size_t i = 0; i < num_events; ++i) {
    auto ec = createEventContext();
    ec->fsevent_stream = stream;
    ec->fsevent_flags = fsevent_flags[i];
    ec->transaction_id = fsevent_ids[i];
    ec->path = std::string(((char**)event_paths)[i]);

    if (ec->fsevent_flags & kFSEventStreamEventFlagMustScanSubDirs) {
      // The FSEvents thread coalesced events within and will report a root.
      TLOG << "FSEvents collision, root: " << ec->path;
    }

    if (ec->fsevent_flags & kFSEventStreamEventFlagRootChanged) {
      // Must rescan for the changed root.
    }

    if (ec->fsevent_flags & kFSEventStreamEventFlagUnmount) {
      // Should remove the watch on this path.
    }

    // Record the string-version of the first matched mask bit.
    bool has_action = false;
    for (const auto& action : kMaskActions) {
      if (ec->fsevent_flags & action.first) {
        // Actions may be multiplexed. Fire and event for each.
        ec->action = action.second;
        EventFactory::fire<FSEventsEventPublisher>(ec);
        has_action = true;
      }
    }

    if (!has_action) {
      // If no action was matched for this path event, fire and unknown.
      ec->action = "UNKNOWN";
      EventFactory::fire<FSEventsEventPublisher>(ec);
    }
  }
}

bool FSEventsEventPublisher::shouldFire(
    const FSEventsSubscriptionContextRef& sc,
    const FSEventsEventContextRef& ec) const {
  if (sc->recursive) {
    // This is stopping us from getting events on links.
    // If we need this feature later, this line will have to be updated to
    // understand links.
    ssize_t found = ec->path.find(sc->path);
    if (found != 0) {
      return false;
    }
  } else if (fnmatch((sc->path + "*").c_str(),
                     ec->path.c_str(),
                     FNM_PATHNAME | FNM_CASEFOLD) != 0) {
    return false;
  }

  if (sc->mask != 0 && !(ec->fsevent_flags & sc->mask)) {
    // Compare the event context mask to the subscription context.
    return false;
  }
  return true;
}

void FSEventsEventPublisher::flush(bool async) {
  if (stream_ != nullptr && stream_started_) {
    if (async) {
      FSEventStreamFlushAsync(stream_);
    } else {
      FSEventStreamFlushSync(stream_);
    }
  }
}

size_t FSEventsEventPublisher::numSubscriptionedPaths() {
  return paths_.size();
}

bool FSEventsEventPublisher::isStreamRunning() {
  if (stream_ == nullptr || !stream_started_) {
    return false;
  }

  if (run_loop_ == nullptr) {
    return false;
  }

  return CFRunLoopIsWaiting(run_loop_);
}
}
