/*
 * BNG Blaster (BBL) - L2TPv2 AVP Functions
 *
 * Christian Giese, February 2022
 *
 * Copyright (C) 2020-2021, RtBrick, Inc.
 */

#include "bbl.h"
#include "bbl_logging.h"
#include <openssl/md5.h>
#include <openssl/rand.h>

/* bbl_l2tp_avp_decode
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |M|H| rsvd  |      Length       |           Vendor ID           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |         Attribute Type        |        Attribute Value...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *                     [until Length is reached]...                |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
static bool
bbl_l2tp_avp_decode(uint8_t **_buf, uint16_t *_len, bbl_l2tp_avp_t *avp) {
    uint8_t *buf = *_buf;
    uint16_t len = *_len;

    if(len < L2TP_AVP_HDR_LEN) {
        return false;
    }
    avp->len = be16toh(*(uint16_t*)buf);
    avp->m = !!(avp->len & L2TP_AVP_M_BIT_MASK);
    avp->h = !!(avp->len & L2TP_AVP_H_BIT_MASK);
    avp->len &= L2TP_AVP_LEN_MASK;
    if(avp->len < L2TP_AVP_HDR_LEN || avp->len > len) {
        return false;
    }
    BUMP_BUFFER(buf, len, sizeof(uint16_t));
    avp->len -= L2TP_AVP_HDR_LEN; /* store value len instead of not AVP len */
    avp->vendor = be16toh(*(uint16_t*)buf);
    BUMP_BUFFER(buf, len, sizeof(uint16_t));
    avp->type = be16toh(*(uint16_t*)buf);
    BUMP_BUFFER(buf, len, sizeof(uint16_t));
    if(avp->len) {
        avp->value = buf;
        BUMP_BUFFER(buf, len, avp->len);
    } else {
        avp->value = NULL;
    }
    *_buf = buf;
    *_len = len;
    return true;
}

/* bbl_l2tp_avp_encode */
static void
bbl_l2tp_avp_encode(uint8_t *buf, uint16_t *len, bbl_l2tp_avp_t *avp) {
    uint16_t avp_len_field;

    /* TODO: Currently we do not support to hide (H) AVP values! */

    avp_len_field = avp->len + L2TP_AVP_HDR_LEN;
    if(avp->m) avp_len_field |= L2TP_AVP_M_BIT_MASK;
    *(uint16_t*)buf = htobe16(len);
    BUMP_WRITE_BUFFER(buf, len, sizeof(uint16_t));
    *(uint16_t*)buf = htobe16(avp->vendor);
    BUMP_WRITE_BUFFER(buf, len, sizeof(uint16_t));
    *(uint16_t*)buf = htobe16(avp->type);
    BUMP_WRITE_BUFFER(buf, len, sizeof(uint16_t));
    switch (avp->value_type) {
        case L2TP_AVP_VALUE_UINT64:
            *(uint64_t*)buf = htobe64(*(uint64_t*)avp->value);
            BUMP_WRITE_BUFFER(buf, len, sizeof(uint64_t));
            break;
        case L2TP_AVP_VALUE_UINT32:
            *(uint32_t*)buf = htobe32(*(uint32_t*)avp->value);
            BUMP_WRITE_BUFFER(buf, len, sizeof(uint32_t));
            break;
        case L2TP_AVP_VALUE_UINT16:
            *(uint16_t*)buf = htobe16(*(uint16_t*)avp->value);
            BUMP_WRITE_BUFFER(buf, len, sizeof(uint16_t));
            break;
        default:
            memcpy(buf, avp->value, avp->len);
            BUMP_WRITE_BUFFER(buf, len, avp->len);
            break;
    }
}

/* bbl_l2tp_avp_unhide */
static bool
bbl_l2tp_avp_unhide(bbl_l2tp_tunnel_t *l2tp_tunnel, bbl_l2tp_avp_t *avp, uint8_t 
                    *random_vector, uint16_t random_vector_len) {
   
    MD5_CTX ctx;
    
    uint8_t  digest[L2TP_MD5_DIGEST_LEN];
    static uint8_t sp[L2TP_AVP_MAX_LEN];

    uint8_t *cursor;
    uint8_t *value = avp->value;
    uint16_t type  = avp->type;
    uint16_t len   = 0;
    uint8_t  idx   = 0;

    char *secret;
    uint16_t secret_len = 0;

    if(!(random_vector && l2tp_tunnel->server->secret)) {
        return false;
    }

    secret = l2tp_tunnel->server->secret;
    secret_len = strlen(secret);

    MD5_Init(&ctx);
    MD5_Update(&ctx, &type, L2TP_AVP_TYPE_LEN);
    MD5_Update(&ctx, secret, secret_len);
    MD5_Update(&ctx, random_vector, random_vector_len);
    MD5_Final(digest, &ctx);

    len = (digest[idx++] ^ *value) << 8;
    value++;
    len = digest[idx++] ^ *value;
    value++;

    if (len + 2 > avp->len) {
        return false;
    }

    avp->len = len;
    avp->value = sp;
    cursor = avp->value;

    while(len) {
        *cursor = digest[idx++] ^ *value;
        len--; value++; cursor++;

        if((idx >= L2TP_MD5_DIGEST_LEN) && len) {
            idx = 0;
            MD5_Init(&ctx);
            MD5_Update(&ctx, secret, secret_len);
            MD5_Update(&ctx, (value-L2TP_MD5_DIGEST_LEN), L2TP_MD5_DIGEST_LEN);
            MD5_Final(digest, &ctx);
        }
    }
    *cursor = 0; // set finale zero
    return true;
}

bool
bbl_l2tp_avp_decode_session(bbl_l2tp_t *l2tp, bbl_l2tp_tunnel_t *l2tp_tunnel, bbl_l2tp_session_t *l2tp_session) {

    uint8_t *buf = l2tp->payload;
    uint16_t len = l2tp->payload_len;
    bbl_l2tp_avp_t avp = {0};
    
    uint8_t *random_vector = NULL;
    uint16_t random_vector_len = 0;

    while(len) {
        if(bbl_l2tp_avp_decode(&buf, &len, &avp)) {
            if(avp.h) {
                if(!bbl_l2tp_avp_unhide(l2tp_tunnel, &avp, random_vector, random_vector_len)) {
                    LOG(L2TP, "L2TP (%s) Failed to decrypt hidden AVP %u in %s from %s\n",
                              l2tp_tunnel->server->host_name, avp.type, 
                              l2tp_message_string(l2tp->type), 
                              format_ipv4_address(&l2tp_tunnel->peer_ip));
                    return false;
                }
            }
            if(avp.vendor == 0) {
                switch(avp.type) {
                    case L2TP_AVP_ASSIGNED_SESSION_ID:
                        if(avp.len != 2) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP assigned session id AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_session_id = be16toh(*(uint16_t*)avp.value);
                        break;
                    case L2TP_AVP_CALL_SERIAL_NUMBER:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP call serial number AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_call_serial_number = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_FRAMING_TYPE:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP framing type AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_framing = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_BEARER_TYPE:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP bearer type AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type),
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_bearer = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_CALLED_NUMBER:
                        if(!l2tp_session->peer_called_number && avp.len) {
                            l2tp_session->peer_called_number = malloc(avp.len + 1);
                            memcpy(l2tp_session->peer_called_number, avp.value, avp.len);
                            *(l2tp_session->peer_called_number + avp.len) = '\0';
                        }
                        break;
                    case L2TP_AVP_CALLING_NUMBER:
                        if(!l2tp_session->peer_calling_number && avp.len) {
                            l2tp_session->peer_calling_number = malloc(avp.len + 1);
                            memcpy(l2tp_session->peer_calling_number, avp.value, avp.len);
                            *(l2tp_session->peer_calling_number + avp.len) = '\0';
                        }
                        break;
                    case L2TP_AVP_SUB_ADDRESS:
                        if(!l2tp_session->peer_sub_address && avp.len) {
                            l2tp_session->peer_sub_address = malloc(avp.len + 1);
                            memcpy(l2tp_session->peer_sub_address, avp.value, avp.len);
                            *(l2tp_session->peer_sub_address + avp.len) = '\0';
                        }
                        break;
                    case L2TP_AVP_TX_CONNECT_SPEED:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP tx connect speed AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type),
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_tx_bps = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_RX_CONNECT_SPEED:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP rx connect speed AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type),
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_rx_bps = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_PHYSICAL_CHANNEL_ID:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP physical channel AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type),
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_physical_channel_id = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_PRIVATE_GROUP_ID:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP private group id AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type),
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->peer_private_group_id = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_SEQUENCING_REQUIRED:
                        l2tp_session->data_sequencing = true;
                        break;
                    case L2TP_AVP_INITIAL_RECEIVED_CONFREQ:
                    case L2TP_AVP_LAST_SENT_CONFREQ:
                    case L2TP_AVP_LAST_RECEIVED_CONFREQ:
                        /* Currently not needed! */
                        break;
                    case L2TP_AVP_PROXY_AUTHEN_TYPE:
                        if(avp.len != 2) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP proxy auth type AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->proxy_auth_type = be16toh(*(uint16_t*)avp.value);
                        break;
                    case L2TP_AVP_PROXY_AUTHEN_NAME:
                        if(!l2tp_session->proxy_auth_name && avp.len) {
                            l2tp_session->proxy_auth_name = malloc(avp.len + 1);
                            memcpy(l2tp_session->proxy_auth_name, avp.value, avp.len);
                            *(l2tp_session->proxy_auth_name + avp.len) = '\0';
                            l2tp_session->proxy_auth_name_len = avp.len;
                        }
                        break;
                    case L2TP_AVP_PROXY_AUTHEN_CHALLENGE:
                        if(!l2tp_session->proxy_auth_challenge && avp.len) {
                            l2tp_session->proxy_auth_challenge = malloc(avp.len);
                            memcpy(l2tp_session->proxy_auth_challenge, avp.value, avp.len);
                            l2tp_session->proxy_auth_challenge_len = avp.len;
                        }
                        break;
                    case L2TP_AVP_PROXY_AUTHEN_ID:
                        if(avp.len != 2) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP proxy auth id AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_session->proxy_auth_id = be16toh(*(uint16_t*)avp.value);
                        break;
                    case L2TP_AVP_PROXY_AUTHEN_RESPONSE:
                        if(!l2tp_session->proxy_auth_response && avp.len) {
                            l2tp_session->proxy_auth_response = malloc(avp.len + 1);
                            memcpy(l2tp_session->proxy_auth_response, avp.value, avp.len);
                            *(l2tp_session->proxy_auth_response + avp.len) = '\0';
                            l2tp_session->proxy_auth_response_len = avp.len;
                        }
                        break;
                    default:
                        if(avp.m) {
                            LOG(L2TP, "L2TP Error (%s) Mandatory standard AVP with unknown type %u in %s from %s\n",
                                    l2tp_tunnel->server->host_name, avp.type, 
                                    l2tp_message_string(l2tp->type), 
                                    format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        } else {
                            LOG(L2TP, "L2TP Warning (%s) Optional standard AVP with unknown type %u in %s from %s\n",
                                    l2tp_tunnel->server->host_name, avp.type, 
                                    l2tp_message_string(l2tp->type), 
                                    format_ipv4_address(&l2tp_tunnel->peer_ip));
                        }
                        break;
                }
            } else {
                if(avp.m) {
                    LOG(L2TP, "L2TP (%s) Mandatory AVP with unknown vendor %u in %s from %s\n",
                              l2tp_tunnel->server->host_name, avp.vendor, 
                              l2tp_message_string(l2tp->type), 
                              format_ipv4_address(&l2tp_tunnel->peer_ip));
                    return false;
                }
            }
        } else {
            LOG(L2TP, "L2TP (%s) Failed to decdoe session attributes in %s from %s\n",
                      l2tp_tunnel->server->host_name, 
                      l2tp_message_string(l2tp->type), 
                      format_ipv4_address(&l2tp_tunnel->peer_ip));
            return false;
        }
    }
    return true;
}                        

bool
bbl_l2tp_avp_decode_tunnel(bbl_l2tp_t *l2tp, bbl_l2tp_tunnel_t *l2tp_tunnel) {

    uint8_t *buf = l2tp->payload;
    uint16_t len = l2tp->payload_len;
    bbl_l2tp_avp_t avp = {0};
    
    uint8_t *random_vector = NULL;
    uint16_t random_vector_len = 0;

    while(len) {
        if(bbl_l2tp_avp_decode(&buf, &len, &avp)) {
            if(avp.h) {
                if(!bbl_l2tp_avp_unhide(l2tp_tunnel, &avp, random_vector, random_vector_len)) {
                    LOG(L2TP, "L2TP (%s) Failed to decrypt hidden AVP %u in %s from %s\n",
                              l2tp_tunnel->server->host_name, avp.type, 
                              l2tp_message_string(l2tp->type), 
                              format_ipv4_address(&l2tp_tunnel->peer_ip));
                    return false;
                }
            }
            if(avp.vendor == 0) {
                switch(avp.type) {
                    case L2TP_AVP_PROTOCOL_VERSION: 
                        if(avp.len != 2 || be16toh(*(uint16_t*)avp.value) != 256) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP protocol version AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        break;
                    case L2TP_AVP_FRAMING_CAPABILITIES:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP framing capabilities AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_tunnel->peer_framing = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_BEARER_CAPABILITIES:
                        if(avp.len != 4) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP bearer capabilities AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type),
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_tunnel->peer_bearer = be32toh(*(uint32_t*)avp.value);
                        break;
                    case L2TP_AVP_FIRMWARE_REVISION:
                        if(avp.len != 2) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP firmware revision AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_tunnel->peer_firmware = be16toh(*(uint16_t*)avp.value);
                        break;
                    case L2TP_AVP_HOST_NAME:
                        if(!l2tp_tunnel->peer_name && avp.len) {
                            l2tp_tunnel->peer_name = malloc(avp.len + 1);
                            memcpy(l2tp_tunnel->peer_name, avp.value, avp.len);
                            *(l2tp_tunnel->peer_name + avp.len) = '\0';
                        }
                        break;
                    case L2TP_AVP_VENDOR_NAME:
                        if(!l2tp_tunnel->peer_vendor && avp.len) {
                            l2tp_tunnel->peer_vendor = malloc(avp.len + 1);
                            memcpy(l2tp_tunnel->peer_vendor, avp.value, avp.len);
                            *(l2tp_tunnel->peer_vendor + avp.len) = '\0';
                        }
                        break;
                    case L2TP_AVP_ASSIGNED_TUNNEL_ID:
                        if(avp.len != 2) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP assigned tunnel id AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_tunnel->peer_tunnel_id = be16toh(*(uint16_t*)avp.value);
                        break;
                    case L2TP_AVP_RECEIVE_WINDOW_SIZE:
                        if(avp.len != 2) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP receive window size AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        l2tp_tunnel->peer_receive_window = be16toh(*(uint16_t*)avp.value);
                        break;
                    case L2TP_AVP_CHALLENGE:
                        if(!l2tp_tunnel->peer_challenge && avp.len) {
                            l2tp_tunnel->peer_challenge = malloc(avp.len);
                            memcpy(l2tp_tunnel->peer_challenge, avp.value, avp.len);
                        }
                        break;
                    case L2TP_AVP_CHALLENGE_RESPONSE:
                        if(avp.len != L2TP_MD5_DIGEST_LEN) {
                            LOG(L2TP, "L2TP Error (%s) Invalid L2TP challenge response AVP in %s from %s\n",
                                      l2tp_tunnel->server->host_name, 
                                      l2tp_message_string(l2tp->type), 
                                      format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        }
                        if(!l2tp_tunnel->peer_challenge_response) {
                            l2tp_tunnel->peer_challenge_response = malloc(avp.len);
                            memcpy(l2tp_tunnel->peer_challenge_response, avp.value, avp.len);
                        }
                        break;
                    default:
                        if(avp.m) {
                            LOG(L2TP, "L2TP Error (%s) Mandatory standard AVP with unknown type %u in %s from %s\n",
                                    l2tp_tunnel->server->host_name, avp.type, 
                                    l2tp_message_string(l2tp->type), 
                                    format_ipv4_address(&l2tp_tunnel->peer_ip));
                            return false;
                        } else {
                            LOG(L2TP, "L2TP Warning (%s) Optional standard AVP with unknown type %u in %s from %s\n",
                                    l2tp_tunnel->server->host_name, avp.type, 
                                    l2tp_message_string(l2tp->type), 
                                    format_ipv4_address(&l2tp_tunnel->peer_ip));
                        }
                        break;
                }
            } else {
                if(avp.m) {
                    LOG(L2TP, "L2TP (%s) Mandatory AVP with unknown vendor %u received from %s\n",
                              l2tp_tunnel->server->host_name, avp.vendor, format_ipv4_address(&l2tp_tunnel->peer_ip));
                    return false;
                }
            }
        } else {
            LOG(L2TP, "L2TP (%s) Failed to decdoe tunnel attributes from %s\n",
                      l2tp_tunnel->server->host_name, format_ipv4_address(&l2tp_tunnel->peer_ip));
            return false;
        }
    }
    return true;
}


void
bbl_l2tp_avp_encode_attributes(bbl_l2tp_tunnel_t *l2tp_tunnel, bbl_l2tp_session_t *l2tp_session, 
                               l2tp_message_type l2tp_type, uint8_t *buf, uint16_t *len) {

    bbl_l2tp_avp_t avp;

    uint16_t v16;
    uint32_t v32;

    if(l2tp_type == L2TP_MESSAGE_ZLB) {
        return;
    }

    /* Protocol Version */
    v16 = l2tp_type;
    avp.m = true;
    if(l2tp_type == L2TP_MESSAGE_CSURQ) {
        avp.m = false;
    }
    avp.type = L2TP_AVP_MESSAGE_TYPE;
    avp.len = 2;
    avp.value_type = L2TP_AVP_VALUE_UINT16;
    avp.value = (void*)&v16;
    bbl_l2tp_avp_encode(buf, len, &avp);

    switch (l2tp_type) {
        case L2TP_MESSAGE_SCCRP:
            /* Protocol Version */
            v16 = 256;
            avp.m = true;
            avp.type = L2TP_AVP_PROTOCOL_VERSION;
            avp.len = 2;
            avp.value_type = L2TP_AVP_VALUE_UINT16;
            avp.value = (void*)&v16;
            bbl_l2tp_avp_encode(buf, len, &avp);
            /* Framing Capabilities */
            v32 = 3; /* A + S */
            avp.m = true;
            avp.type = L2TP_AVP_FRAMING_CAPABILITIES;
            avp.len = 4;
            avp.value_type = L2TP_AVP_VALUE_UINT32;
            avp.value = (void*)&v32;
            /* Bearer Capabilities */
            bbl_l2tp_avp_encode(buf, len, &avp);
            v32 = 1; /* D */
            avp.m = true;
            avp.type = L2TP_AVP_BEARER_CAPABILITIES;
            avp.len = 4;
            avp.value_type = L2TP_AVP_VALUE_UINT32;
            avp.value = (void*)&v32;
            bbl_l2tp_avp_encode(buf, len, &avp);
            /* Firmware Revision */
            v16 = 1;
            avp.m = false;
            avp.type = L2TP_AVP_BEARER_CAPABILITIES;
            avp.len = 2;
            avp.value_type = L2TP_AVP_VALUE_UINT16;
            avp.value = (void*)&v16;
            bbl_l2tp_avp_encode(buf, len, &avp);
            /* Host Name */
            avp.m = true;
            avp.type = L2TP_AVP_HOST_NAME;
            avp.len = strlen(l2tp_tunnel->server->host_name);
            avp.value_type = L2TP_AVP_VALUE_BYTES;
            avp.value = (void*)(l2tp_tunnel->server->host_name);
            bbl_l2tp_avp_encode(buf, len, &avp);
            /* Vendor Name */
            avp.m = false;
            avp.type = L2TP_AVP_VENDOR_NAME;
            avp.len = strlen(l2tp_tunnel->server->vendor);
            avp.value_type = L2TP_AVP_VALUE_BYTES;
            avp.value = (void*)(l2tp_tunnel->server->vendor);
            bbl_l2tp_avp_encode(buf, len, &avp);
            /* Assigned Tunnel ID  */
            avp.m = true;
            avp.type = L2TP_AVP_ASSIGNED_TUNNEL_ID;
            avp.len = 2;
            avp.value_type = L2TP_AVP_VALUE_UINT16;
            avp.value = (void*)(&l2tp_tunnel->tunnel_id);
            bbl_l2tp_avp_encode(buf, len, &avp);
            /* Receive Window Size  */
            avp.m = true;
            avp.type = L2TP_AVP_RECEIVE_WINDOW_SIZE;
            avp.len = 2;
            avp.value_type = L2TP_AVP_VALUE_UINT16;
            avp.value = (void*)(&l2tp_tunnel->server->receive_window);
            bbl_l2tp_avp_encode(buf, len, &avp);
            /* Challenge */
            if(l2tp_tunnel->challenge_len) {
                avp.m = false;
                avp.type = L2TP_AVP_CHALLENGE;
                avp.len = l2tp_tunnel->challenge_len;
                avp.value_type = L2TP_AVP_VALUE_BYTES;
                avp.value = l2tp_tunnel->challenge;
                bbl_l2tp_avp_encode(buf, len, &avp);
            }
            /* Challenge Response */
            if(l2tp_tunnel->challenge_response_len) {
                avp.m = false;
                avp.type = L2TP_AVP_CHALLENGE_RESPONSE;
                avp.len = l2tp_tunnel->challenge_response_len;
                avp.value_type = L2TP_AVP_VALUE_BYTES;
                avp.value = l2tp_tunnel->challenge_response;
                bbl_l2tp_avp_encode(buf, len, &avp);
            }
            break;
        case L2TP_MESSAGE_STOPCCN:
            /* Assigned Tunnel ID  */
            avp.m = true;
            avp.type = L2TP_AVP_ASSIGNED_TUNNEL_ID;
            avp.len = 2;
            avp.value_type = L2TP_AVP_VALUE_UINT16;
            avp.value = (void*)(&l2tp_tunnel->tunnel_id);
            bbl_l2tp_avp_encode(buf, len, &avp);
            break;
        case L2TP_MESSAGE_ICRP:
            /* Assigned Session ID  */
            if(l2tp_session) {
                avp.m = true;
                avp.type = L2TP_AVP_ASSIGNED_SESSION_ID;
                avp.len = 2;
                avp.value_type = L2TP_AVP_VALUE_UINT16;
                avp.value = (void*)(&l2tp_session->key.session_id);
                bbl_l2tp_avp_encode(buf, len, &avp);
            }
            break;
        case L2TP_MESSAGE_CDN:
            /* Assigned Session ID  */
            if(l2tp_session) {
                avp.m = true;
                avp.type = L2TP_AVP_ASSIGNED_SESSION_ID;
                avp.len = 2;
                avp.value_type = L2TP_AVP_VALUE_UINT16;
                avp.value = (void*)(&l2tp_session->key.session_id);
                bbl_l2tp_avp_encode(buf, len, &avp);
            }
            break;
        default:
            break;
    }
}