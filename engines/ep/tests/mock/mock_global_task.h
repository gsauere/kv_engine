/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include "globaltask.h"

#include <string>

/**
 * Mock Task class. Doesn't actually run() or snooze() - they both do nothing.
 */
class MockGlobalTask : public GlobalTask {
public:
    MockGlobalTask(Taskable& t, TaskId id) : GlobalTask(t, id) {
    }

    bool run() override {
        return false;
    }
    std::string getDescription() override {
        return "MockGlobalTask";
    }

    std::chrono::microseconds maxExpectedDuration() override {
        // Shouldn't matter what this returns
        return std::chrono::seconds(0);
    }

    void snooze(const double secs) override {
    }
};
