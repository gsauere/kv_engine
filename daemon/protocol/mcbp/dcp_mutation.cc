/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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

#include "dcp_mutation.h"
#include "engine_wrapper.h"
#include "utilities.h"
#include "../../mcbp.h"

#include <platform/compress.h>
#include <limits>
#include <stdexcept>
#include <xattr/blob.h>
#include <xattr/utils.h>

ENGINE_ERROR_CODE dcp_message_mutation(const void* void_cookie,
                                       uint32_t opaque,
                                       item* it,
                                       uint16_t vbucket,
                                       uint64_t by_seqno,
                                       uint64_t rev_seqno,
                                       uint32_t lock_time,
                                       const void* meta,
                                       uint16_t nmeta,
                                       uint8_t nru,
                                       uint8_t collection_len) {
    if (void_cookie == nullptr) {
        throw std::invalid_argument(
                "dcp_message_deletion: void_cookie can't be nullptr");
    }
    const auto& ccookie = *static_cast<const Cookie*>(void_cookie);
    auto& cookie = const_cast<Cookie&>(ccookie);
    auto* c = &cookie.getConnection();

    // Use a unique_ptr to make sure we release the item in all error paths
    cb::unique_item_ptr item(it, cb::ItemDeleter{c->getBucketEngineAsV0()});

    item_info info;
    if (!bucket_get_item_info(cookie, it, &info)) {
        LOG_WARNING(c, "%u: Failed to get item info", c->getId());
        return ENGINE_FAILED;
    }

    char* root = reinterpret_cast<char*>(info.value[0].iov_base);
    cb::char_buffer buffer{root, info.value[0].iov_len};

    if (!c->reserveItem(it)) {
        LOG_WARNING(c, "%u: Failed to grow item array", c->getId());
        return ENGINE_FAILED;
    }

    // we've reserved the item, and it'll be released when we're done sending
    // the item.
    item.release();
    protocol_binary_request_dcp_mutation packet(c->isDcpCollectionAware(),
                                                opaque,
                                                vbucket,
                                                info.cas,
                                                info.nkey,
                                                buffer.len,
                                                info.datatype,
                                                by_seqno,
                                                rev_seqno,
                                                info.flags,
                                                info.exptime,
                                                lock_time,
                                                nmeta,
                                                nru,
                                                collection_len);

    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;
    c->write->produce([&c, &packet, &info, &buffer, &meta, &nmeta, &ret](
                              cb::byte_buffer wbuf) -> size_t {

        const size_t packetlen =
                protocol_binary_request_dcp_mutation::getHeaderLength(
                        c->isDcpCollectionAware());

        if (wbuf.size() < (packetlen + nmeta)) {
            ret = ENGINE_E2BIG;
            return 0;
        }

        std::copy(packet.bytes, packet.bytes + packetlen, wbuf.begin());

        if (nmeta > 0) {
            std::copy(static_cast<const uint8_t*>(meta),
                      static_cast<const uint8_t*>(meta) + nmeta,
                      wbuf.data() + packetlen);
        }

        // Add the header
        c->addIov(wbuf.data(), packetlen);

        // Add the key
        c->addIov(info.key, info.nkey);

        // Add the value
        c->addIov(buffer.buf, buffer.len);

        // Add the optional meta section
        if (nmeta > 0) {
            c->addIov(wbuf.data() + packetlen, nmeta);
        }

        return packetlen + nmeta;
    });

    return ret;
}

static inline ENGINE_ERROR_CODE do_dcp_mutation(Cookie& cookie) {
    auto packet = cookie.getPacket(Cookie::PacketContent::Full);
    auto& connection = cookie.getConnection();
    const auto* req =
            reinterpret_cast<const protocol_binary_request_dcp_mutation*>(
                    packet.data());

    // Collection aware DCP will be sending the collection_len field
    auto body_offset = protocol_binary_request_dcp_mutation::getHeaderLength(
            connection.isDcpCollectionAware());

    // Namespace defaults to DefaultCollection for legacy DCP
    DocNamespace ns = DocNamespace::DefaultCollection;
    if (connection.isDcpCollectionAware() &&
        req->message.body.collection_len != 0) {
        // Collection aware DCP sends non-zero collection_len for documents that
        // are in collections.
        ns = DocNamespace::Collections;
    }

    const uint16_t nkey = ntohs(req->message.header.request.keylen);
    const DocKey key{req->bytes + body_offset, nkey, ns};

    const auto opaque = req->message.header.request.opaque;
    const auto datatype = req->message.header.request.datatype;
    const uint64_t cas = ntohll(req->message.header.request.cas);
    const uint16_t vbucket = ntohs(req->message.header.request.vbucket);
    const uint64_t by_seqno = ntohll(req->message.body.by_seqno);
    const uint64_t rev_seqno = ntohll(req->message.body.rev_seqno);
    const uint32_t flags = req->message.body.flags;
    const uint32_t expiration = ntohl(req->message.body.expiration);
    const uint32_t lock_time = ntohl(req->message.body.lock_time);
    const uint16_t nmeta = ntohs(req->message.body.nmeta);
    const uint32_t valuelen = ntohl(req->message.header.request.bodylen) -
                              nkey - req->message.header.request.extlen -
                              nmeta;
    cb::const_byte_buffer value{req->bytes + body_offset + nkey, valuelen};
    cb::const_byte_buffer meta{value.buf + valuelen, nmeta};
    uint32_t priv_bytes = 0;
    if (mcbp::datatype::is_xattr(datatype)) {
        cb::const_char_buffer payload{reinterpret_cast<const char*>(value.buf),
                                      value.len};
        cb::byte_buffer buffer{const_cast<uint8_t*>(value.buf),
                               cb::xattr::get_body_offset(payload)};
        cb::xattr::Blob blob(buffer);
        priv_bytes = uint32_t(blob.get_system_size());
        if (priv_bytes > COUCHBASE_MAX_ITEM_PRIVILEGED_BYTES) {
            return ENGINE_E2BIG;
        }
    }

    auto engine = connection.getBucketEngine();
    return engine->dcp.mutation(connection.getBucketEngineAsV0(),
                                &cookie,
                                opaque,
                                key,
                                value,
                                priv_bytes,
                                datatype,
                                cas,
                                vbucket,
                                flags,
                                by_seqno,
                                rev_seqno,
                                expiration,
                                lock_time,
                                meta,
                                req->message.body.nru);
}

void dcp_mutation_executor(Cookie& cookie) {
    ENGINE_ERROR_CODE ret = cookie.getAiostat();
    cookie.setAiostat(ENGINE_SUCCESS);
    cookie.setEwouldblock(false);

    auto& connection = cookie.getConnection();
    if (ret == ENGINE_SUCCESS) {
        ret = do_dcp_mutation(cookie);
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        connection.setState(McbpStateMachine::State::new_cmd);
        break;

    case ENGINE_DISCONNECT:
        connection.setState(McbpStateMachine::State::closing);
        break;

    case ENGINE_EWOULDBLOCK:
        cookie.setEwouldblock(true);
        break;

    default:
        cookie.sendResponse(cb::engine_errc(ret));
    }
}
