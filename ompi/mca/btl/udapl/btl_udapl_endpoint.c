/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */


#include "ompi_config.h"
#include <sys/time.h>
#include <time.h>
#include "ompi/types.h"
#include "orte/mca/ns/base/base.h"
#include "orte/mca/oob/base/base.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/dss/dss.h"
#include "ompi/mca/mpool/udapl/mpool_udapl.h"
#include "btl_udapl.h"
#include "btl_udapl_endpoint.h" 
#include "btl_udapl_proc.h"
#include "btl_udapl_frag.h"


static void mca_btl_udapl_endpoint_send_cb(int status, orte_process_name_t* endpoint, 
        orte_buffer_t* buffer, orte_rml_tag_t tag, void* cbdata);
static int mca_btl_udapl_start_connect(mca_btl_base_endpoint_t* endpoint);
static int mca_btl_udapl_endpoint_post_recv(mca_btl_udapl_endpoint_t* endpoint);
void mca_btl_udapl_endpoint_connect(mca_btl_udapl_endpoint_t* endpoint);
void mca_btl_udapl_endpoint_recv(int status, orte_process_name_t* endpoint, 
        orte_buffer_t* buffer, orte_rml_tag_t tag, void* cbdata);

 
int mca_btl_udapl_endpoint_send(mca_btl_base_endpoint_t* endpoint,
                                mca_btl_udapl_frag_t* frag)
{
    int rc = OMPI_SUCCESS;
    DAT_DTO_COOKIE cookie;

    OPAL_THREAD_LOCK(&endpoint->endpoint_send_lock);
    switch(endpoint->endpoint_state) {
        case MCA_BTL_UDAPL_CONNECTED:
            /* just send it already.. */
            OPAL_OUTPUT((0, "sending %d bytes\n", frag->triplet.segment_length));
            cookie.as_ptr = frag;
            rc = dat_ep_post_send(endpoint->endpoint_ep, 1, &frag->triplet,
                    cookie, DAT_COMPLETION_DEFAULT_FLAG);
            if(DAT_SUCCESS != rc) {
                MCA_BTL_UDAPL_ERROR(rc, "dat_ep_post_send");
                rc = OMPI_ERROR;
            }

            break;
        case MCA_BTL_UDAPL_CLOSED:
            /* Initiate a new connection, add this send to a queue */
            rc = mca_btl_udapl_start_connect(endpoint);
            if(OMPI_SUCCESS != rc) {
                endpoint->endpoint_state = MCA_BTL_UDAPL_FAILED;
                break;
            }

            /* Fall through on purpose to queue the send */
        case MCA_BTL_UDAPL_CONNECTING:
            /* Add this send to a queue */
            opal_list_append(&endpoint->endpoint_frags,
                             (opal_list_item_t*)frag);
            break;
        case MCA_BTL_UDAPL_FAILED:
            rc = OMPI_ERR_UNREACH;
            break;
    }
    OPAL_THREAD_UNLOCK(&endpoint->endpoint_send_lock);

    return rc;
}


static void mca_btl_udapl_endpoint_send_cb(int status, orte_process_name_t* endpoint, 
        orte_buffer_t* buffer, orte_rml_tag_t tag, void* cbdata)
{
    OBJ_RELEASE(buffer);
}


static int mca_btl_udapl_start_connect(mca_btl_base_endpoint_t* endpoint)
{
    mca_btl_udapl_addr_t* addr = &endpoint->endpoint_btl->udapl_addr;
    orte_buffer_t* buf = OBJ_NEW(orte_buffer_t);
    int rc;

    if(NULL == buf) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return ORTE_ERR_OUT_OF_RESOURCE;
    }

    /* Pack our address information */
    rc = orte_dss.pack(buf, &addr->port, 1, ORTE_UINT64);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    rc = orte_dss.pack(buf, &addr->addr, sizeof(DAT_SOCK_ADDR), ORTE_UINT8);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    /* Send the buffer */
    rc = orte_rml.send_buffer_nb(&endpoint->endpoint_proc->proc_guid, buf,
            ORTE_RML_TAG_DYNAMIC - 1, 0, mca_btl_udapl_endpoint_send_cb, NULL);
    if(0 > rc) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    endpoint->endpoint_state = MCA_BTL_UDAPL_CONNECTING;
    return OMPI_SUCCESS;
}


void mca_btl_udapl_endpoint_recv(int status, orte_process_name_t* endpoint, 
        orte_buffer_t* buffer, orte_rml_tag_t tag, void* cbdata)
{
    mca_btl_udapl_addr_t addr;
    mca_btl_udapl_proc_t* proc;
    mca_btl_base_endpoint_t* ep;
    size_t cnt = 1;
    size_t i;
    int rc;

    /* Unpack data */
    rc = orte_dss.unpack(buffer, &addr.port, &cnt, ORTE_UINT64);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    cnt = sizeof(mca_btl_udapl_addr_t);
    rc = orte_dss.unpack(buffer, &addr.addr, &cnt, ORTE_UINT8);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* Match the endpoint and handle it */
    OPAL_THREAD_LOCK(&mca_btl_udapl_component.udapl_lock);
    for(proc = (mca_btl_udapl_proc_t*)
                opal_list_get_first(&mca_btl_udapl_component.udapl_procs);
            proc != (mca_btl_udapl_proc_t*)
                opal_list_get_end(&mca_btl_udapl_component.udapl_procs);
            proc  = (mca_btl_udapl_proc_t*)opal_list_get_next(proc)) {

        if(0 == orte_ns.compare(ORTE_NS_CMP_ALL, &proc->proc_guid, endpoint)) {
            for(i = 0; i < proc->proc_endpoint_count; i++) {
                ep = proc->proc_endpoints[i];
            
                /* Does this endpoint match? */
                if(!memcmp(&addr, &ep->endpoint_addr,
                        sizeof(mca_btl_udapl_addr_t))) {
                    OPAL_OUTPUT((0, "btl_udapl found endpoint!\n"));
                    OPAL_THREAD_UNLOCK(&mca_btl_udapl_component.udapl_lock);
                    mca_btl_udapl_endpoint_connect(ep);
                    return;
                }
            }
        }
    }
    OPAL_THREAD_UNLOCK(&mca_btl_udapl_component.udapl_lock);
}


/*
 * Set up OOB recv callback.
 */

void mca_btl_udapl_endpoint_post_oob_recv(void)
{
    orte_rml.recv_buffer_nb(ORTE_RML_NAME_ANY, ORTE_RML_TAG_DYNAMIC-1,
            ORTE_RML_PERSISTENT, mca_btl_udapl_endpoint_recv, NULL);
}


void mca_btl_udapl_endpoint_connect(mca_btl_udapl_endpoint_t* endpoint)
{
    mca_btl_udapl_module_t* btl = endpoint->endpoint_btl;
    mca_btl_udapl_frag_t* frag;
    DAT_DTO_COOKIE cookie;
    int rc;

    OPAL_THREAD_LOCK(&endpoint->endpoint_send_lock);

    /* Nasty test to prevent deadlock and unwanted connection attempts */
    /* This right here is the whole point of using the ORTE/RML handshake */
    if((MCA_BTL_UDAPL_CONNECTING == endpoint->endpoint_state &&
            0 > orte_ns.compare(ORTE_NS_CMP_ALL,
                    &endpoint->endpoint_proc->proc_guid,
                    &ompi_proc_local()->proc_name)) ||
            (MCA_BTL_UDAPL_CLOSED != endpoint->endpoint_state &&
             MCA_BTL_UDAPL_CONNECTING != endpoint->endpoint_state)) {
        OPAL_THREAD_UNLOCK(&endpoint->endpoint_send_lock);
        return;
    }

    /* Create a new uDAPL endpoint and start the connection process */
    rc = dat_ep_create(btl->udapl_ia, btl->udapl_pz,
            btl->udapl_evd_dto, btl->udapl_evd_dto, btl->udapl_evd_conn,
            NULL, &endpoint->endpoint_ep);
    if(DAT_SUCCESS != rc) {
        MCA_BTL_UDAPL_ERROR(rc, "dat_ep_create");
        goto failure_create;
    }

    rc = dat_ep_connect(endpoint->endpoint_ep, &endpoint->endpoint_addr.addr,
            endpoint->endpoint_addr.port, mca_btl_udapl_component.udapl_timeout,
            0, NULL, 0, DAT_CONNECT_DEFAULT_FLAG);
    if(DAT_SUCCESS != rc) {
        MCA_BTL_UDAPL_ERROR(rc, "dat_ep_connect");
        goto failure;
    }

    /* Send our local address data over this EP */
    /* Can't use btl_udapl_send here, will start an infinite loop! */
    frag = (mca_btl_udapl_frag_t*)mca_btl_udapl_alloc(
            (mca_btl_base_module_t*)btl, sizeof(mca_btl_udapl_addr_t));
    cookie.as_ptr = frag;

    memcpy(frag->segment.seg_addr.pval,
            &btl->udapl_addr, sizeof(mca_btl_udapl_addr_t));
    frag->endpoint = endpoint;
    frag->type = MCA_BTL_UDAPL_CONN_SEND;


    /* Do the actual send now.. */
    rc = dat_ep_post_send(endpoint->endpoint_ep, 1,
            &frag->triplet, cookie, DAT_COMPLETION_DEFAULT_FLAG);
    if(DAT_SUCCESS != rc) {
        MCA_BTL_UDAPL_ERROR(rc, "dat_ep_post_send");
        goto failure;
    }

    endpoint->endpoint_state = MCA_BTL_UDAPL_CONNECTING;
    OPAL_THREAD_UNLOCK(&endpoint->endpoint_send_lock);
    return;

failure:
    dat_ep_free(endpoint->endpoint_ep);
failure_create:
    endpoint->endpoint_ep = DAT_HANDLE_NULL;
    endpoint->endpoint_state = MCA_BTL_UDAPL_FAILED;
    OPAL_THREAD_UNLOCK(&endpoint->endpoint_send_lock);
    return;
}


/*
 * Post queued sends.
 */

int mca_btl_udapl_endpoint_post_queue(mca_btl_udapl_endpoint_t* endpoint)
{
    mca_btl_udapl_frag_t* frag;
    DAT_DTO_COOKIE cookie;
    int rc = OMPI_SUCCESS;

    OPAL_THREAD_LOCK(&endpoint->endpoint_send_lock);
    endpoint->endpoint_state = MCA_BTL_UDAPL_CONNECTED;
    while(NULL != (frag = (mca_btl_udapl_frag_t*)
            opal_list_remove_first(&endpoint->endpoint_frags))) {
        cookie.as_ptr = frag;
        rc = dat_ep_post_send(endpoint->endpoint_ep, 1,
                &frag->triplet, cookie, DAT_COMPLETION_DEFAULT_FLAG);
        if(DAT_SUCCESS != rc) {
            MCA_BTL_UDAPL_ERROR(rc, "dat_ep_post_send");
            rc = OMPI_ERROR;
            break;
        }
    }
    OPAL_THREAD_UNLOCK(&endpoint->endpoint_send_lock);

    return mca_btl_udapl_endpoint_post_recv(endpoint);
}


/*
 * Match a uDAPL endpoint to a BTL endpoint.
 */

int mca_btl_udapl_endpoint_match(struct mca_btl_udapl_module_t* btl,
                                 mca_btl_udapl_addr_t* addr,
                                 DAT_EP_HANDLE endpoint)
{
    mca_btl_udapl_proc_t* proc;
    mca_btl_base_endpoint_t* ep;
    size_t i;

    OPAL_THREAD_LOCK(&mca_btl_udapl_component.udapl_lock);
    for(proc = (mca_btl_udapl_proc_t*)
                opal_list_get_first(&mca_btl_udapl_component.udapl_procs);
            proc != (mca_btl_udapl_proc_t*)
                opal_list_get_end(&mca_btl_udapl_component.udapl_procs);
            proc  = (mca_btl_udapl_proc_t*)opal_list_get_next(proc)) {
    
        for(i = 0; i < proc->proc_endpoint_count; i++) {
            ep = proc->proc_endpoints[i];
    
            /* Does this endpoint match? */
            if(ep->endpoint_btl == btl &&
                    !memcmp(addr, &ep->endpoint_addr,
                            sizeof(mca_btl_udapl_addr_t))) {
                OPAL_OUTPUT((0, "btl_udapl matched endpoint!\n"));
                ep->endpoint_ep = endpoint;
                mca_btl_udapl_endpoint_post_queue(ep);
                OPAL_THREAD_UNLOCK(&mca_btl_udapl_component.udapl_lock);
                return OMPI_SUCCESS;
            }
        }
    }
    OPAL_THREAD_UNLOCK(&mca_btl_udapl_component.udapl_lock);

    /* If this point is reached, no matching endpoint was found */
    OPAL_OUTPUT((0, "btl_udapl ERROR could not match endpoint\n"));
    return OMPI_ERROR;
}


/*
 * Post receive buffers for a newly established endpoint connection.
 */

static int mca_btl_udapl_endpoint_post_recv(mca_btl_udapl_endpoint_t* endpoint)
{
    mca_btl_udapl_frag_t* frag;
    DAT_DTO_COOKIE cookie;
    int rc;
    int i;

    /* TODO - only posting eager frags for now. */
    OPAL_THREAD_LOCK(&endpoint->endpoint_recv_lock);
    for(i = 0; i < mca_btl_udapl_component.udapl_num_repost; i++) {
        MCA_BTL_UDAPL_FRAG_ALLOC_EAGER(endpoint->endpoint_btl, frag, rc);
    
        /* Set up the LMR triplet from the frag segment */
        /* Note that this triplet defines a sub-region of a registered LMR */
        frag->triplet.virtual_address = (DAT_VADDR)frag->hdr;
        frag->triplet.segment_length =
            mca_btl_udapl_module.super.btl_eager_limit;
    
        frag->btl = endpoint->endpoint_btl;
        frag->endpoint = endpoint;
        frag->base.des_dst = &frag->segment;
        frag->base.des_dst_cnt = 1;
        frag->type = MCA_BTL_UDAPL_RECV;

        cookie.as_ptr = frag;

        rc = dat_ep_post_recv(endpoint->endpoint_ep, 1,
                &frag->triplet, cookie, DAT_COMPLETION_DEFAULT_FLAG);
        if(DAT_SUCCESS != rc) {
            MCA_BTL_UDAPL_ERROR(rc, "dat_ep_post_recv");
            OPAL_THREAD_UNLOCK(&endpoint->endpoint_recv_lock);
            return OMPI_ERROR;
        }
    }
    OPAL_THREAD_UNLOCK(&endpoint->endpoint_recv_lock);

    return OMPI_SUCCESS;
}


/*
 * Initialize state of the endpoint instance.
 *
 */

static void mca_btl_udapl_endpoint_construct(mca_btl_base_endpoint_t* endpoint)
{
    endpoint->endpoint_btl = 0;
    endpoint->endpoint_proc = 0;
    endpoint->endpoint_state = MCA_BTL_UDAPL_CLOSED;
    endpoint->endpoint_ep = DAT_HANDLE_NULL;

    OBJ_CONSTRUCT(&endpoint->endpoint_frags, opal_list_t);
    OBJ_CONSTRUCT(&endpoint->endpoint_send_lock, opal_mutex_t);
    OBJ_CONSTRUCT(&endpoint->endpoint_recv_lock, opal_mutex_t);
}


/*
 * Destroy a endpoint
 *
 */

static void mca_btl_udapl_endpoint_destruct(mca_btl_base_endpoint_t* endpoint)
{
    OBJ_DESTRUCT(&endpoint->endpoint_frags);
    OBJ_DESTRUCT(&endpoint->endpoint_send_lock);
    OBJ_DESTRUCT(&endpoint->endpoint_recv_lock);
}


OBJ_CLASS_INSTANCE(
    mca_btl_udapl_endpoint_t, 
    opal_list_item_t, 
    mca_btl_udapl_endpoint_construct, 
    mca_btl_udapl_endpoint_destruct);

