//  Copyright (c) 2014 University of Oregon
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifdef APEX_HAVE_HPX3
#include <hpx/config.hpp>
#endif

#include "apex_api.hpp"
#include "apex.hpp"
#include "profiler_listener.hpp"
#include "profiler.hpp"
#include "thread_instance.hpp"
#include "semaphore.hpp"
#include <iostream>
#include <fstream>
#include <math.h>
#include "apex_options.hpp"
#include "profiler.hpp"
#include "profile.hpp"

#include <boost/thread/thread.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/atomic.hpp>
#include <atomic>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <boost/assign.hpp>
#include <boost/cstdint.hpp>
#if !defined(_WIN32) && (defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__)))
#include <unistd.h>
#include <sched.h>
#endif
#include <cstdio>
#include <vector>
#include <string>
#include <boost/regex.hpp>

#if defined(APEX_THROTTLE)
#define APEX_THROTTLE_CALLS 1000
#define APEX_THROTTLE_PERCALL 0.00001 // 10 microseconds.
#endif
#include <unordered_set>

#if APEX_HAVE_BFD
#include "address_resolution.hpp"
#endif

#if APEX_HAVE_PAPI
#include "papi.h"
#endif

#ifdef APEX_HAVE_HPX3
#include <hpx/include/performance_counters.hpp>
#include <hpx/include/actions.hpp>
#include <hpx/include/util.hpp>
#include <hpx/lcos/local/composable_guard.hpp>
#endif

//#define MAX_QUEUE_SIZE 1024*1024
//#define MAX_QUEUE_SIZE 1024
#define MAX_QUEUE_SIZE 4096
#define INITIAL_NUM_THREADS 2

#define APEX_MAIN "APEX MAIN"

#ifdef APEX_HAVE_TAU
#define PROFILING_ON
#define TAU_DOT_H_LESS_HEADERS
#include <TAU.h>
#endif

using namespace std;
using namespace apex;

APEX_NATIVE_TLS unsigned int my_tid = 0; // the current thread's TID in APEX

namespace apex {

  /* This is the array of profiler queues, one for each worker thread. It
   * is initialized to a length of 8, there is code in on_new_thread() to
   * increment it if necessary.  */
  std::vector<boost::lockfree::spsc_queue<profiler*>* > profiler_queues(2);

#if APEX_HAVE_PAPI
  std::vector<int> event_sets(8);
#endif

#ifndef APEX_HAVE_HPX3
  /* This is the thread that will read from the queues of profiler
   * objects, and update the profiles. */
  boost::thread * consumer_thread;
#endif

  /* Flag indicating that we are done inserting into the queues, so the
   * consumer knows when to exit */
  boost::atomic<bool> done (false);

#ifndef APEX_HAVE_HPX3
  /* how the workers signal the consumer that there are new profiler objects
   * on the queue to consume */
  semaphore queue_signal;
#endif

#ifdef APEX_HAVE_HPX3
  /* Flag indicating whether a consumer task is currently running */
  std::atomic_flag consumer_task_running = ATOMIC_FLAG_INIT;
#endif

  /* The profiles, mapped by name. Profiles will be in this map or the other
   * one, not both. It depends whether the timers are identified by name
   * or address. */
  static map<string, profile*> name_map;
  static vector<map<string, profile*>* > thread_name_maps(INITIAL_NUM_THREADS);

  /* The profiles, mapped by address */
  static map<apex_function_address, profile*> address_map;
  vector<map<apex_function_address, profile*>* > thread_address_maps(INITIAL_NUM_THREADS);

  /* If we are throttling, keep a set of addresses that should not be timed.
   * We also have a set of names. */
#if defined(APEX_THROTTLE)
  static unordered_set<apex_function_address> throttled_addresses;
  static unordered_set<string> throttled_names;
#endif

  /* measurement of entire application */
  profiler * profiler_listener::main_timer(nullptr);

  /* the node id is needed for profile output. */
  int profiler_listener::node_id(0);

  /* A lock necessary for registering new threads */
  boost::mutex profiler_listener::_mtx ;

  /* This is our garbage collection. This listener could be done with the profiler
   * object, but the tau_listener may not be. So don't delete it until the tau_listener
   * is done with it. */
  static unordered_set<profiler*> my_garbage;

#if APEX_HAVE_PAPI
  static int num_papi_counters = 0;
#endif

  /* Return the requested profile object to the user.
   * Return nullptr if doesn't exist. */
  profile * profiler_listener::get_profile(apex_function_address address) {
    map<apex_function_address, profile*>::const_iterator it = address_map.find(address);
    if (it != address_map.end()) {
      return (*it).second;
    }
    return nullptr;
  }

  /* Return the requested profile object to the user.
   * Return nullptr if doesn't exist. */
  profile * profiler_listener::get_profile(const string &timer_name) {
    map<string, profile*>::const_iterator it = name_map.find(timer_name);
    if (it != name_map.end()) {
      return (*it).second;
    }
    return nullptr;
  }

  /* Return a vector of all name-based profiles */
  std::vector<std::string> profiler_listener::get_available_profiles() {
    std::vector<std::string> names;
    boost::copy(name_map | boost::adaptors::map_keys, std::back_inserter(names));
    return names;
  }

  void reset_all() {
    for(auto &it : name_map) {
        it.second->reset();
    }
    for(auto &it : address_map) {
        it.second->reset();
    }
    if (apex_options::use_profile_output() > 1) {
        for(auto &it1 : *(thread_name_maps[my_tid])) {
            it1.second->reset();
        }
        for(auto &it1 : *(thread_address_maps[my_tid])) {
            it1.second->reset();
        }
    }
  }

  /* After the consumer thread pulls a profiler off of the queue,
   * process it by updating its profile object in the map of profiles. */
  // TODO The name-based timer and address-based timer paths through
  // the code involve a lot of duplication -- this should be refactored
  // to remove the duplication so it's easier to maintain.
  inline unsigned int profiler_listener::process_profile(profiler * p, unsigned int tid)
  {
    if(p == nullptr) return 0;
    profile * theprofile;
    if(p->is_reset == reset_type::ALL) {
        reset_all();
        if(p->safe_to_delete) {
            delete(p);
        } else {
            my_garbage.insert(p);
        }
        return 0;
    }
    // Look for the profile object by name, if applicable
    if (p->have_name) {
      map<string, profile*>::const_iterator it = name_map.find(*(p->timer_name));
      if (it != name_map.end()) {
        // A profile for this name already exists.
        theprofile = (*it).second;
        if(p->is_reset == reset_type::CURRENT) {
            theprofile->reset();
        } else {
            theprofile->increment(p->elapsed(), p->is_resume);
        }
#if defined(APEX_THROTTLE)
        // Is this a lightweight task? If so, we shouldn't measure it any more,
        // in order to reduce overhead.
        if (theprofile->get_calls() > APEX_THROTTLE_CALLS &&
            theprofile->get_mean() < APEX_THROTTLE_PERCALL) {
          unordered_set<string>::const_iterator it = throttled_names.find(p->timer_name);
          if (it == throttled_names.end()) {
            throttled_names.insert(p->timer_name);
            cout << "APEX Throttled " << p->timer_name << endl; fflush;
          }
        }
#endif
      } else {
        // Create a new profile for this name.
        theprofile = new profile(p->is_reset == reset_type::CURRENT ? 0.0 : p->elapsed(), p->is_resume, p->is_counter ? APEX_COUNTER : APEX_TIMER);
        name_map[*(p->timer_name)] = theprofile;
#ifdef APEX_HAVE_HPX3
#ifdef APEX_REGISTER_HPX3_COUNTERS
        if(!done) {
            if(get_hpx_runtime_ptr() != nullptr) {
                std::string timer_name(*(p->timer_name));
                //Don't register timers containing "/"
                if(timer_name.find("/") == std::string::npos) {
                    hpx::performance_counters::install_counter_type(
                    std::string("/apex/") + timer_name,
                    [p](bool r)->boost::int64_t{
                        boost::int64_t value(p->elapsed() * 100000);
                        return value;
                    },
                    std::string("APEX counter ") + timer_name,
                    ""
                    );
                }
            } else {
                std::cerr << "HPX runtime not initialized yet." << std::endl;
            }
        }
#endif
#endif
      }
      if (apex_options::use_profile_output() > 1) {
        // now do thread-specific measurement.
        map<string, profile*>* the_map = thread_name_maps[tid];
        it = the_map->find(*(p->timer_name));
        if (it != the_map->end()) {
            // A profile for this name already exists.
            theprofile = (*it).second;
            if(p->is_reset == reset_type::CURRENT) {
                theprofile->reset();
            } else {
                theprofile->increment(p->elapsed(), p->is_resume);
            }
        } else {
            // Create a new profile for this name.
            theprofile = new profile(p->is_reset == reset_type::CURRENT ? 0.0 : p->elapsed(), p->is_resume, p->is_counter ? APEX_COUNTER : APEX_TIMER);
            (*the_map)[*(p->timer_name)] = theprofile;
        }
      }
    } else { // address rather than name
      map<apex_function_address, profile*>::const_iterator it2 = address_map.find(p->action_address);
      if (it2 != address_map.end()) {
        // A profile for this name already exists.
        theprofile = (*it2).second;
        if(p->is_reset == reset_type::CURRENT) {
            theprofile->reset();
        } else {
            theprofile->increment(p->elapsed(), p->is_resume);
        }
#if defined(APEX_THROTTLE)
        // Is this a lightweight task? If so, we shouldn't measure it any more,
        // in order to reduce overhead.
        if (theprofile->get_calls() > APEX_THROTTLE_CALLS &&
            theprofile->get_mean() < APEX_THROTTLE_PERCALL) {
          unordered_set<apex_function_address>::const_iterator it = throttled_addresses.find(p->action_address);
          if (it == throttled_addresses.end()) {
            throttled_addresses.insert(p->action_address);
#if defined(HAVE_BFD)
            cout << "APEX Throttled " << *(lookup_address((uintptr_t)p->action_address, true)) << endl;
#else
            cout << "APEX Throttled " << p->action_address << endl;
#endif
          }
        }
#endif
      } else {
        // Create a new profile for this address.
        theprofile = new profile(p->is_reset == reset_type::CURRENT ? 0.0 : p->elapsed(), p->is_resume);
        address_map[p->action_address] = theprofile;
      }
      if (apex_options::use_profile_output() > 1) {
        // now do thread-specific measurement
        map<apex_function_address, profile*>* the_map = thread_address_maps[tid];
        it2 = the_map->find(p->action_address);
        if (it2 != the_map->end()) {
            // A profile for this name already exists.
            theprofile = (*it2).second;
            if(p->is_reset == reset_type::CURRENT) {
                theprofile->reset();
            } else {
                theprofile->increment(p->elapsed(), p->is_resume);
            }
        } else {
            // Create a new profile for this address.
            theprofile = new profile(p->is_reset == reset_type::CURRENT ? 0.0 : p->elapsed(), p->is_resume);
            (*the_map)[p->action_address] = theprofile;
        }
      }
    }
    // done with the profiler object
    //if(p->safe_to_delete) {
        //delete(p);
    //} else {
        my_garbage.insert(p);
    //}
    return 1;
  }

  /* Cleaning up memory. Not really necessary, because it only gets
   * called at shutdown. But a good idea to do regardless. */
  void profiler_listener::delete_profiles(void) {
    // iterate over the map and free the objects in the map
    map<apex_function_address, profile*>::const_iterator it;
    for(it = address_map.begin(); it != address_map.end(); it++) {
      delete it->second;
    }
    // iterate over the map and free the objects in the map
    map<string, profile*>::const_iterator it2;
    for(it2 = name_map.begin(); it2 != name_map.end(); it2++) {
      delete it2->second;
    }
    // clear the maps.
    address_map.clear();
    name_map.clear();
    // iterate over the queues, and delete them
    unsigned int i = 0;
    for (i = 0 ; i < profiler_queues.size(); i++) {
      if (profiler_queues[i]) {
        delete (profiler_queues[i]);
      }
    }
    // clear the vector of queues.
    profiler_queues.clear();

    if (apex_options::use_profile_output() > 1) {
        // iterate over the vector of profile objects for per-thread measurement
        for (i = 0 ; i < thread_address_maps.size(); i++) {
        if (thread_address_maps[i]) {
            for(it = thread_address_maps[i]->begin(); it != thread_address_maps[i]->end(); it++) {
            delete it->second;
            }
            delete (thread_address_maps[i]);
        }
        }
        // clear the vector of maps.
        thread_address_maps.clear();

        // iterate over the vector of profile objects for per-thread measurement
        for (i = 0 ; i < thread_name_maps.size(); i++) {
        if (thread_name_maps[i]) {
            for(it2 = thread_name_maps[i]->begin(); it2 != thread_name_maps[i]->end(); it2++) {
            delete it2->second;
            }
            delete (thread_name_maps[i]);
        }
        }
        // clear the vector of maps.
        thread_name_maps.clear();
    }
  }

  /* At program termination, write the measurements to the screen. */
  void profiler_listener::finalize_profiles(void) {
    // iterate over the profiles in the address map
    map<apex_function_address, profile*>::const_iterator it;
    cout << "Action, #calls, min, mean, max, total, stddev" << endl;
    for(it = address_map.begin(); it != address_map.end(); it++) {
      profile * p = it->second;
      apex_function_address function_address = it->first;
#if defined(APEX_THROTTLE)
      // if this profile was throttled, don't output the measurements.
      // they are limited and bogus, anyway.
      unordered_set<apex_function_address>::const_iterator it = throttled_addresses.find(function_address);
      if (it != throttled_addresses.end()) { continue; }
#endif
#if APEX_HAVE_BFD
      // translate the address to a name
      string * tmp = lookup_address((uintptr_t)function_address, true);
      string shorter(*tmp);
      // to keep formatting pretty, trim any long timer names
      if (shorter.size() > 30) {
        shorter.resize(27);
        shorter.resize(30, '.');
      }
      cout << "\"" << shorter << "\", " ;
#else
      cout << "\"" << function_address << "\", " ;
#endif
      cout << p->get_calls() << ", " ;
      cout << p->get_minimum() << ", " ;
      cout << p->get_mean() << ", " ;
      cout << p->get_maximum() << ", " ;
      cout << p->get_accumulated() << ", " ;
      cout << p->get_stddev() << endl;
    }
    map<string, profile*>::const_iterator it2;
    // iterate over the profiles in the name map
    for(it2 = name_map.begin(); it2 != name_map.end(); it2++) {
      profile * p = it2->second;
      string action_name = it2->first;
#if defined(APEX_THROTTLE)
      // if this profile was throttled, don't output the measurements.
      // they are limited and bogus, anyway.
      unordered_set<apex_function_address>::const_iterator it = throttled_names.find(action_name);
      if (it != throttled_names.end()) { continue; }
#endif
#if APEX_HAVE_BFD
      boost::regex rx (".*UNRESOLVED ADDR (.*)");
      if (boost::regex_match (action_name,rx)) {
        const boost::regex separator(" ADDR ");
        boost::sregex_token_iterator token(action_name.begin(), action_name.end(), separator, -1);
        *token++; // ignore
        string addr_str = *token++;
	void* addr_addr;
	sscanf(addr_str.c_str(), "%p", &addr_addr);
        string * tmp = lookup_address((uintptr_t)addr_addr, true);
        boost::regex old_address("UNRESOLVED ADDR " + addr_str);
	action_name = boost::regex_replace(action_name, old_address, *tmp);
      }
#endif
      string shorter(action_name);
      // to keep formatting pretty, trim any long timer names
      if (shorter.size() > 30) {
        shorter.resize(27);
        shorter.resize(30, '.');
      }
      cout << "\"" << shorter << "\", " ;
      if(p->get_calls() < 1) {
        p->get_profile()->calls = 1;
      }
      cout << p->get_calls() << ", " ;
      cout << p->get_minimum() << ", " ;
      cout << p->get_mean() << ", " ;
      cout << p->get_maximum() << ", " ;
      cout << p->get_accumulated() << ", " ;
      cout << p->get_stddev() << endl;
    }
  }

  /* When writing a TAU profile, write out a timer line */
  void format_line(ofstream &myfile, profile * p) {
    myfile << p->get_calls() << " ";
    myfile << 0 << " ";
    myfile << (p->get_accumulated() * 1000000.0) << " ";
    myfile << (p->get_accumulated() * 1000000.0) << " ";
    myfile << 0 << " ";
    myfile << "GROUP=\"TAU_USER\" ";
    myfile << endl;
  }

  /* When writing a TAU profile, write out the main timer line */
  void format_line(ofstream &myfile, profile * p, double not_main) {
    myfile << p->get_calls() << " ";
    myfile << 0 << " ";
    myfile << (max((p->get_accumulated() - not_main),0.0) * 1000000.0) << " ";
    myfile << (p->get_accumulated() * 1000000.0) << " ";
    myfile << 0 << " ";
    myfile << "GROUP=\"TAU_USER\" ";
    myfile << endl;
  }

  /* When writing a TAU profile, write out a counter line */
  void format_counter_line(ofstream &myfile, profile * p) {
    myfile << p->get_calls() << " ";       // numevents
    myfile << p->get_maximum() << " ";     // max
    myfile << p->get_minimum() << " ";     // min
    myfile << p->get_mean() << " ";        // mean
    myfile << p->get_sum_squares() << " ";
    myfile << endl;
  }

  /* Write TAU profiles from the collected data. */
  void profiler_listener::write_profile(int tid) {
    ofstream myfile;
    stringstream datname;
    map<string, profile*>* the_name_map = nullptr;
    map<apex_function_address, profile*>* the_address_map = nullptr;

    if (tid == -1) {
      the_name_map = &name_map;
      the_address_map = &address_map;
      // name format: profile.nodeid.contextid.threadid
      // We only write one profile per process
      datname << "profile." << node_id << ".0.0";
    } else {
      if (thread_name_maps[tid] == nullptr || thread_address_maps[tid] == nullptr) return;
      the_name_map = thread_name_maps[tid];
      the_address_map = thread_address_maps[tid];
      // name format: profile.nodeid.contextid.threadid
      datname << "profile." << node_id << ".0." << tid;
    }

    // name format: profile.nodeid.contextid.threadid
    myfile.open(datname.str().c_str());
    int counter_events = 0;

    // Determine number of counter events, as these need to be
    // excluded from the number of normal timers
    map<string, profile*>::const_iterator it2;
    for(it2 = the_name_map->begin(); it2 != the_name_map->end(); it2++) {
      profile * p = it2->second;
      if(p->get_type() == APEX_COUNTER) {
        counter_events++;
      }
    }
    int function_count = the_address_map->size() + (the_name_map->size() - counter_events);

    // Print the normal timers to the profile file
    // 1504 templated_functions_MULTI_TIME
    myfile << function_count << " templated_functions_MULTI_TIME" << endl;
    // # Name Calls Subrs Excl Incl ProfileCalls #
    myfile << "# Name Calls Subrs Excl Incl ProfileCalls #" << endl;
    thread_instance ti = thread_instance::instance();

    // Iterate over the profiles which are associated to a function
    // by address. All of these are regular timers.
    map<apex_function_address, profile*>::const_iterator it;
    for(it = the_address_map->begin(); it != the_address_map->end(); it++) {
      profile * p = it->second;
      // ".TAU application" 1 8 8658984 8660739 0 GROUP="TAU_USER"
      apex_function_address function_address = it->first;
#if APEX_HAVE_BFD
      string * tmp = lookup_address((uintptr_t)function_address, true);
      myfile << "\"" << *tmp << "\" ";
#else
      myfile << "\"" << ti.map_addr_to_name(function_address) << "\" ";
#endif
      format_line (myfile, p);
    }

    // Iterate over the profiles which are associated to a function
    // by name. Only output the regular timers now. Counters are
    // in a separate section, below.
    profile * mainp = nullptr;
    double not_main = 0.0;
    for(it2 = the_name_map->begin(); it2 != the_name_map->end(); it2++) {
      profile * p = it2->second;
      if(p->get_type() == APEX_TIMER) {
        string action_name = it2->first;
        if(strcmp(action_name.c_str(), APEX_MAIN) == 0) {
          mainp = p;
        } else {
#if APEX_HAVE_BFD
      boost::regex rx (".*UNRESOLVED ADDR (.*)");
      if (boost::regex_match (action_name,rx)) {
        const boost::regex separator(" ADDR ");
        boost::sregex_token_iterator token(action_name.begin(), action_name.end(), separator, -1);
        *token++; // ignore
        string addr_str = *token++;
	void* addr_addr;
	sscanf(addr_str.c_str(), "%p", &addr_addr);
        string * tmp = lookup_address((uintptr_t)addr_addr, true);
        boost::regex old_address("UNRESOLVED ADDR " + addr_str);
	action_name = boost::regex_replace(action_name, old_address, *tmp);
      }
#endif
          myfile << "\"" << action_name << "\" ";
          format_line (myfile, p);
          not_main += p->get_accumulated();
        }
      }
    }
    if (mainp != nullptr) {
      myfile << "\"" << APEX_MAIN << "\" ";
      format_line (myfile, mainp, not_main);
    }

    // 0 aggregates
    myfile << "0 aggregates" << endl;

    // Now process the counters, if there are any.
    if(counter_events > 0) {
      myfile << counter_events << " userevents" << endl;
      myfile << "# eventname numevents max min mean sumsqr" << endl;
      for(it2 = the_name_map->begin(); it2 != the_name_map->end(); it2++) {
        profile * p = it2->second;
        if(p->get_type() == APEX_COUNTER) {
          string action_name = it2->first;
          myfile << "\"" << action_name << "\" ";
          format_counter_line (myfile, p);
        }
      }
    }
    myfile.close();
  }


  /* This is the main function for the consumer thread.
   * It will wait at a semaphore for pending work. When there is
   * work on one or more queues, it will iterate over the queues
   * and process the pending profiler objects, updating the profiles
   * as it goes. */
  void profiler_listener::process_profiles(void)
  {
    static bool _initialized = false;
    if (!_initialized) {
      initialize_worker_thread_for_TAU();
      _initialized = true;
    }
#ifdef APEX_HAVE_TAU
    if (apex_options::use_tau()) {
      TAU_START("profiler_listener::process_profiles");
    }
#endif

    profiler * p;
    unsigned int i;
    // Main loop. Stay in this loop unless "done".
#ifndef APEX_HAVE_HPX3
    while (!done) {
      queue_signal.wait();
#endif
#ifdef APEX_HAVE_TAU
      /*
    if (apex_options::use_tau()) {
      TAU_START("profiler_listener::process_profiles: main loop");
    }
    */
#endif
      //unsigned int processed = 0;
      //do { // inner while loop, handle all the available work while there is work.
        //processed = 0;
        for (i = 0 ; i < profiler_queues.size(); i++) {
            if (profiler_queues[i]) {
                while (profiler_queues[i]->pop(p)) {
                    //processed += process_profile(p, i);
                    process_profile(p, i);
                }
            }
        }
        // do some garbage collection
        for (std::unordered_set<profiler*>::const_iterator itr = my_garbage.begin(); itr != my_garbage.end();) {
            profiler* tmp = *itr;
            assert (tmp != nullptr);
            //if (tmp != nullptr) {
                if (tmp->safe_to_delete) {
                    my_garbage.erase(itr++);
                    delete(tmp);
                } else {
                    ++itr;
                }
            //}
        }
      //} while (!done && processed > 0);
#ifdef USE_UDP
      // are we updating a global profile?
      if (apex_options::use_udp_sink()) {
          udp_client::synchronize_profiles(name_map, address_map);
      }
#endif
#ifdef APEX_HAVE_TAU
      /*
      if (apex_options::use_tau()) {
        TAU_STOP("profiler_listener::process_profiles: main loop");
      }
      */
#endif
#ifndef APEX_HAVE_HPX3
    }

    // We might be done, but check to make sure the queues are empty
    for (i = 0 ; i < profiler_queues.size(); i++) {
      if (profiler_queues[i]) {
        while (profiler_queues[i]->pop(p)) {
          process_profile(p, i);
        }
      }
    }

    // stop the main timer, and process that profile
    main_timer->stop();
    process_profile(main_timer, my_tid);

#ifdef USE_UDP
    // are we updating a global profile?
    if (apex_options::use_udp_sink()) {
        udp_client::synchronize_profiles(name_map, address_map);
        udp_client::stop_client();
    }
#endif

#endif // NOT DEFINED APEX_HAVE_HPX3

#ifdef APEX_HAVE_HPX3
    consumer_task_running.clear(memory_order_release);
#endif

#ifdef APEX_HAVE_TAU
    if (apex_options::use_tau()) {
      TAU_STOP("profiler_listener::process_profiles");
    }
#endif
  }

#ifdef APEX_HAVE_HPX3
} // end namespace apex (HPX_PLAIN_ACTION needs to be in global namespace)

HPX_PLAIN_ACTION(profiler_listener::process_profiles, apex_internal_process_profiles_action);
HPX_ACTION_HAS_CRITICAL_PRIORITY(apex_internal_process_profiles_action);

namespace apex {

void profiler_listener::schedule_process_profiles() {
    if(get_hpx_runtime_ptr() == nullptr) return;
    if(!consumer_task_running.test_and_set(memory_order_acq_rel)) {
        apex_internal_process_profiles_action act;
        try {
            hpx::apply(act, hpx::find_here());
        } catch(...) {
            // During shutdown, we can't schedule a new task,
            // so we process profiles ourselves.
            profiler_listener::process_profiles();
        }
    } 
}
#endif

#if APEX_HAVE_PAPI
APEX_NATIVE_TLS int EventSet = PAPI_NULL;
#define PAPI_ERROR_CHECK(name) \
if (rc != 0) cout << "name: " << rc << ": " << PAPI_strerror(rc) << endl;

  void initialize_PAPI(bool first_time) {
      int rc = 0;
      if (first_time) {
        PAPI_library_init( PAPI_VER_CURRENT );
        rc = PAPI_multiplex_init(); // use more counters than allowed
        PAPI_ERROR_CHECK(PAPI_multiplex_init);
        PAPI_thread_init( thread_instance::get_id );
        rc = PAPI_set_domain(PAPI_DOM_ALL);
        PAPI_ERROR_CHECK(PAPI_set_domain);
      }
      rc = PAPI_create_eventset(&EventSet);
      PAPI_ERROR_CHECK(PAPI_create_eventset);
      rc = PAPI_assign_eventset_component (EventSet, 0);
      PAPI_ERROR_CHECK(PAPI_assign_eventset_component);
      rc = PAPI_set_granularity(PAPI_GRN_THR);
      PAPI_ERROR_CHECK(PAPI_set_granularity);
      rc = PAPI_set_multiplex(EventSet);
      PAPI_ERROR_CHECK(PAPI_set_multiplex);
      if (PAPI_query_event (PAPI_TOT_CYC) == PAPI_OK) {
        rc = PAPI_add_event( EventSet, PAPI_TOT_CYC);
        PAPI_ERROR_CHECK(PAPI_add_event);
        num_papi_counters++;
      }
      if (PAPI_query_event (PAPI_TOT_INS) == PAPI_OK) {
        rc = PAPI_add_event( EventSet, PAPI_TOT_INS);
        PAPI_ERROR_CHECK(PAPI_add_event);
        num_papi_counters++;
      }
      if (PAPI_query_event (PAPI_L2_TCM) == PAPI_OK) {
        rc = PAPI_add_event( EventSet, PAPI_L2_TCM);
        PAPI_ERROR_CHECK(PAPI_add_event);
        num_papi_counters++;
      }
      if (PAPI_query_event (PAPI_BR_MSP) == PAPI_OK) {
        rc = PAPI_add_event( EventSet, PAPI_BR_MSP);
        PAPI_ERROR_CHECK(PAPI_add_event);
        num_papi_counters++;
      }
      if (PAPI_query_event (PAPI_FP_INS) == PAPI_OK) {
        //rc = PAPI_add_event( EventSet, PAPI_FP_OPS);
        rc = PAPI_add_event( EventSet, PAPI_FP_INS);
        PAPI_ERROR_CHECK(PAPI_add_event);
        num_papi_counters++;
      }
      rc = PAPI_start( EventSet );
      PAPI_ERROR_CHECK(PAPI_start);
  }

#endif

  /* When APEX gets a STARTUP event, do some initialization. */
  void profiler_listener::on_startup(startup_event_data &data) {
    if (!_terminate) {
      // Create a profiler queue for this main thread
      profiler_queues[0] = new boost::lockfree::spsc_queue<profiler*>(MAX_QUEUE_SIZE);
      if (apex_options::use_profile_output() > 1) {
        thread_address_maps[0] = new map<apex_function_address, profile*>();
        thread_name_maps[0] = new map<string, profile*>();
      }
#ifndef APEX_HAVE_HPX3
      // Start the consumer thread, to process profiler objects.
      consumer_thread = new boost::thread(process_profiles);
#endif

#if APEX_HAVE_PAPI
      initialize_PAPI(true);
      event_sets[0] = EventSet;
#endif

      /* This commented out code is to change the priority of the consumer thread.
       * IDEALLY, I would like to make this a low priority thread, but that is as
       * yet unsuccessful. */
#if 0
      int retcode;
      int policy;

      pthread_t threadID = (pthread_t) consumer_thread->native_handle();

      struct sched_param param;

      if ((retcode = pthread_getschedparam(threadID, &policy, &param)) != 0)
      {
        errno = retcode;
        perror("pthread_getschedparam");
        exit(EXIT_FAILURE);
      }
      std::cout << "INHERITED: ";
      std::cout << "policy=" << ((policy == SCHED_FIFO)  ? "SCHED_FIFO" :
          (policy == SCHED_RR)    ? "SCHED_RR" :
          (policy == SCHED_OTHER) ? "SCHED_OTHER" :
          "???")
        << ", priority=" << param.sched_priority << " of " << sched_get_priority_min(policy) << "," << sched_get_priority_max(policy)  << std::endl;
      //param.sched_priority = 10;
      if ((retcode = pthread_setschedparam(threadID, policy, &param)) != 0)
      {
        errno = retcode;
        perror("pthread_setschedparam");
        exit(EXIT_FAILURE);
      }
#endif

      // time the whole application.
      main_timer = new profiler(new string(APEX_MAIN));
    }
	APEX_UNUSED(data);
  }

  /* On the shutdown event, notify the consumer thread that we are done
   * and set the "terminate" flag. */
  void profiler_listener::on_shutdown(shutdown_event_data &data) {
    if (_terminate) { return; }
    _mtx.lock();
    if (!_terminate) {
      node_id = data.node_id;
      _terminate = true;
      done = true;
      //sleep(1);
#ifndef APEX_HAVE_HPX3
      queue_signal.post();
      if (consumer_thread != nullptr) {
          consumer_thread->join();
          delete consumer_thread;
      }
#endif

/* no longer necessary - the exit_thread() will do this for us.
 * If we do this now, we will end up with two main timers, and the
 * possibility that one of them will crash in post-processing. */
#if 0 
    // stop the main timer, and process that profile
    if (main_timer != nullptr) {
        main_timer->stop();
        process_profile(main_timer, my_tid);
        delete main_timer;
    }
#endif

    // output to screen?
    if (apex_options::use_screen_output() && node_id == 0)
    {
      finalize_profiles();
    }

    // output to 1 TAU profile per process?
    if (apex_options::use_profile_output() == 1)
    {
      write_profile(-1);
    }
    // output to TAU profiles, one per thread per process?
    else if (apex_options::use_profile_output() > 1)
    {
      // the number of thread_name_maps tells us how many threads there are to process
      for (unsigned int i = 0 ; i < thread_name_maps.size(); i++) {
        write_profile((int)i);
      }
    }

#if APEX_HAVE_PAPI
      int rc = 0;
      int i = 0;
      long long values[8] {0L};
      //cout << values[0] << " " << values[1] << " " << values[2] << " " << values[3] << endl;
      for (i = 0 ; i < thread_instance::get_num_threads() ; i++) {
        rc = PAPI_accum( event_sets[i], values );
        PAPI_ERROR_CHECK(PAPI_stop);
        //cout << values[0] << " " << values[1] << " " << values[2] << " " << values[3] << endl;
      }
      if (apex_options::use_screen_output() && node_id == 0) {
        cout << endl << "TOTAL COUNTERS for " << thread_instance::get_num_threads() << " threads:" << endl;
        cout << "Cycles: " << values[0] ;
        cout << ", Instructions: " << values[1] ;
        cout << ", L2TCM: " << values[2] ;
        if (num_papi_counters > 3) {
            cout << ", BR_MSP: " << values[3] ;
        }
        if (num_papi_counters > 4) {
            cout << ", FPINS: " << values[4] ;
            //cout << ", FPOPS: " << values[4] ;
        }

        cout << endl << "IPC: " << (double)(values[1])/(double)(values[0]) ;
        cout << endl << "INS/L2TCM: " << (double)(values[1])/(double)(values[2]) ;
        if (num_papi_counters > 3) {
            cout << endl << "INS/BR_MSP: " << (double)(values[1])/(double)(values[3]) ;
        }
        if (num_papi_counters > 4) {
            cout << endl << "FLINS%INS: " << (double)(values[4])/(double)(values[1]) ;
            //cout << endl << "FLOP%INS: " << (double)(values[4])/(double)(values[1]) ;
        }
        cout << endl;
      }
#endif
    }
    /* The cleanup is disabled for now. Why? Because we want to be able
     * to access the profiles at the end of the run, after APEX has
     * finalized. */
    // cleanup.
    // delete_profiles();
    _mtx.unlock();
  }

  /* When a new node is created */
  void profiler_listener::on_new_node(node_event_data &data) {
    if (!_terminate) {
    }
	APEX_UNUSED(data);
  }

  /* When a new thread is registered, expand all of our storage as necessary
   * to handle the new thread */
  void profiler_listener::on_new_thread(new_thread_event_data &data) {
    if (!_terminate) {
      _mtx.lock();
      // resize the vector
      my_tid = (unsigned int)thread_instance::get_id();
      if (my_tid >= profiler_queues.size()) {
        profiler_queues.resize(my_tid + 1);
      }
      if (apex_options::use_profile_output() > 1) {
        if (my_tid >= thread_address_maps.size()) {
            thread_address_maps.resize(my_tid + 1);
        }
        if (my_tid >= thread_name_maps.size()) {
            thread_name_maps.resize(my_tid + 1);
        }
      }
      unsigned int i = 0;
      // allocate the queue(s)
      for (i = 0; i < my_tid+1 ; i++) {
        if (profiler_queues[i] == nullptr) {
          boost::lockfree::spsc_queue<profiler*>* tmp =
            new boost::lockfree::spsc_queue<profiler*>(MAX_QUEUE_SIZE);
          profiler_queues[i] = tmp;
        }
        if (apex_options::use_profile_output() > 1) {
            if (thread_address_maps[i] == nullptr) {
                thread_address_maps[i] = new map<apex_function_address, profile*>();
            }
            if (thread_name_maps[i] == nullptr) {
                thread_name_maps[i] = new map<string, profile*>();
            }
        }
      }
#if APEX_HAVE_PAPI
      initialize_PAPI(false);
      if (my_tid >= event_sets.size()) {
        event_sets.resize(my_tid + 1);
      }
      event_sets[my_tid] = EventSet;
#endif
      _mtx.unlock();
    }
	APEX_UNUSED(data);
  }

  /* When a start event happens, create a profiler object. Unless this
   * named event is throttled, in which case do nothing, as quickly as possible */
  inline void profiler_listener::_common_start(apex_function_address function_address, bool is_resume) {
    if (!_terminate) {
#if defined(APEX_THROTTLE)
        // if this timer is throttled, return without doing anything
        unordered_set<apex_function_address>::const_iterator it = throttled_addresses.find(data.function_address);
        if (it != throttled_addresses.end()) {
          thread_instance::instance().set_current_profiler(nullptr);
          return;
        }
#endif
        // start the profiler object, which starts our timers
        thread_instance::instance().set_current_profiler(new profiler(function_address, is_resume));
#if APEX_HAVE_PAPI
        long long * values = thread_instance::instance().get_current_profiler()->papi_start_values;
        int rc = 0;
        rc = PAPI_read( EventSet, values );
        PAPI_ERROR_CHECK(PAPI_read);
#endif
    }
  }

  /* When a start event happens, create a profiler object. Unless this
   * named event is throttled, in which case do nothing, as quickly as possible */
  inline void profiler_listener::_common_start(std::string * timer_name, bool is_resume) {
    if (!_terminate) {
#if defined(APEX_THROTTLE)
      // if this timer is throttled, return without doing anything
      unordered_set<apex_function_address>::const_iterator it = throttled_names.find(*timer_name);
      if (it != throttled_names.end()) {
        thread_instance::instance().set_current_profiler(nullptr);
        return;
      }
#endif
      // start the profiler object, which starts our timers
      thread_instance::instance().set_current_profiler(new profiler(timer_name, is_resume));
#if APEX_HAVE_PAPI
      long long * values = thread_instance::instance().get_current_profiler()->papi_start_values;
      int rc = 0;
      rc = PAPI_read( EventSet, values );
      PAPI_ERROR_CHECK(PAPI_read);
#endif
    }
  }

  inline void profiler_listener::push_profiler(int my_tid, profiler *p) {
      assert(profiler_queues[my_tid]);
      bool worked = profiler_queues[my_tid]->push(p);
      if (!worked) {
          static bool issued = false;
          if (!issued) {
              issued = true;
              if(p->have_name) {
                  cout << "APEX Warning : failed to push " << *(p->timer_name) << endl;
              } else {
#if defined(HAVE_BFD)
                  cout << "APEX Warning : failed to push " << *(lookup_address((uintptr_t)p->action_address, true) << endl;
#else
                  cout << "APEX Warning : failed to push address " << p->action_address << endl;
#endif
              }
              cout << "One or more frequently-called, lightweight functions is being timed." << endl;
          }
      }
#ifndef APEX_HAVE_HPX3
      queue_signal.post();
#endif
#ifdef APEX_HAVE_HPX3
      schedule_process_profiles();
#endif
  }

  /* Stop the timer, if applicable, and queue the profiler object */
  inline void profiler_listener::_common_stop(profiler * p, bool is_yield) {
    if (!_terminate) {
      if (p) {
        p->stop(is_yield);
#if APEX_HAVE_PAPI
        long long * values = p->papi_stop_values;
        int rc = 0;
        rc = PAPI_read( EventSet, values );
        PAPI_ERROR_CHECK(PAPI_read);
#endif
        push_profiler(my_tid, p);
      }
    }
  }

  /* Start the timer */
  void profiler_listener::on_start(apex_function_address function_address) {
    _common_start(function_address, false);
  }

  /* Start the timer */
  void profiler_listener::on_start(std::string * timer_name) {
    _common_start(timer_name, false);
  }

  /* This is just like starting a timer, but don't increment the number of calls
   * value. That is because we are restarting an existing timer. */
  void profiler_listener::on_resume(std::string * timer_name) {
        _common_start(timer_name, true);
  }

  /* This is just like starting a timer, but don't increment the number of calls
   * value. That is because we are restarting an existing timer. */
  void profiler_listener::on_resume(apex_function_address function_address) {
        _common_start(function_address, true);
  }

   /* Stop the timer */
  void profiler_listener::on_stop(profiler * p) {
    _common_stop(p, false);
  }

  /* Stop the timer, but don't increment the number of calls */
  void profiler_listener::on_yield(profiler * p) {
    _common_stop(p, true);
  }

  /* When a thread exits, pop and stop all timers. */
  void profiler_listener::on_exit_thread(event_data &data) {
    if (!_terminate) {
        profiler *p = nullptr;
        while(true) {
            try {
                p = thread_instance::instance().pop_current_profiler();
            } catch (empty_stack_exception& e) { break; }
            if (p != nullptr) {
                _common_stop(p, false);
                // don't delete the profiler, it will be deleted when
                // the consumer thread processes it.
            }
        }
    }
    APEX_UNUSED(data);
  }

  /* When a sample value is processed, save it as a profiler object, and queue it. */
  void profiler_listener::on_sample_value(sample_value_event_data &data) {
    if (!_terminate) {
      profiler * p = new profiler(new string(*data.counter_name), data.counter_value);
      p->is_counter = data.is_counter;
      push_profiler(my_tid, p);
      p->safe_to_delete = true;
    }
  }

  /* For periodic stuff. Do something? */
  void profiler_listener::on_periodic(periodic_event_data &data) {
    if (!_terminate) {
    }
	APEX_UNUSED(data);
  }

  /* For custom event stuff. Do something? */
  void profiler_listener::on_custom_event(custom_event_data &data) {
    if (!_terminate) {
    }
	APEX_UNUSED(data);
  }

  void profiler_listener::reset(apex_function_address function_address) {
    profiler * p;
    if(function_address != APEX_NULL_FUNCTION_ADDRESS) {
    p = new profiler(function_address, false, reset_type::CURRENT);
    } else {
    p = new profiler((apex_function_address)APEX_NULL_FUNCTION_ADDRESS, false, reset_type::ALL);
    }
    push_profiler(my_tid, p);
  }

  void profiler_listener::reset(const std::string &timer_name) {
    profiler * p;
    p = new profiler(new string(timer_name), false, reset_type::CURRENT);
    push_profiler(my_tid, p);
  }

  profiler_listener::~profiler_listener (void) { 
#ifdef USE_UDP
      if (apex_options::use_udp_sink()) {
          udp_client::stop_client();
      }
#endif
      delete_profiles();
  };

}
