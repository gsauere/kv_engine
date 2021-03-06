/*
 *     Copyright 2019 Couchbase, Inc
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

#include "clustertest.h"

#include "cluster.h"

#include <nlohmann/json.hpp>

std::unique_ptr<cb::test::Cluster> cb::test::ClusterTest::cluster;

void cb::test::ClusterTest::SetUpTestCase() {
    cluster = Cluster::create(4);
    if (!cluster) {
        std::cerr << "Failed to create the cluster" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    auto bucket = cluster->createBucket("default",
                                        {{"replicas", 2}, {"max_vbuckets", 8}});
    if (!bucket) {
        std::cerr << "Failed to create bucket default" << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void cb::test::ClusterTest::TearDownTestCase() {
    cluster.reset();
}

void cb::test::ClusterTest::SetUp() {
    Test::SetUp();
}

void cb::test::ClusterTest::TearDown() {
    Test::TearDown();
}
