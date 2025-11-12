/*
 * nvme_cpl.c - NVMe Completion Queue Handling
 *
 * Functions for processing completion queue entries.
 */

#include "nvmedrv.h"

/*
 * Helper: Set SCSI adapter error with specific status and SCSI status
 */
void
nvme_set_adapter_status(scsi_request_t *req, uint_t sr_status, u_char sr_scsi_status)
{
    req->sr_status = sr_status;
    req->sr_scsi_status = sr_scsi_status;
    req->sr_resid = req->sr_buflen;
    req->sr_sensegotten = 0;
}

/*
 * Helper: Set SCSI adapter error (for internal driver errors)
 */
void
nvme_set_adapter_error(scsi_request_t *req)
{
    nvme_set_adapter_status(req, SC_REQUEST, ST_CHECK);
}

/*
 * Helper: Set SCSI success
 */
void
nvme_set_success(scsi_request_t *req)
{
    req->sr_status = SC_GOOD;
    req->sr_scsi_status = ST_GOOD;
    req->sr_resid = 0;
    req->sr_sensegotten = 0;
}

void
nvme_read_completion(nvme_completion_t *cpl, nvme_queue_t *q)
{
    uint_t *dest = (uint_t *)cpl;
    volatile uint_t *src = (volatile uint_t *)&q->cq[q->cq_head & q->size_mask];

#ifdef IP30
    heart_dcache_inval((caddr_t)src, sizeof(nvme_completion_t));
#endif
    dest[0] = NVME_MEMRD(&src[0]);
    dest[1] = NVME_MEMRD(&src[1]);
    dest[2] = NVME_MEMRD(&src[2]);
    dest[3] = NVME_MEMRD(&src[3]);
}

/*
 * nvme_process_completions: Process all pending completions in a CQ
 * returns number of processed completions
 *
 * IMPORTANT: This function does NOT hold q->lock because:
 * 1. Only ONE completion processor runs at a time (interrupt XOR timeout)
 * 2. cq_head is only modified here (single-threaded)
 * 3. sq_head is written here but only read by submitters (who hold lock)
 * 4. sr_notify() MUST be called without ANY locks held (IRIX requirement)
 */
int
nvme_process_completions(nvme_soft_t *soft, nvme_queue_t *q)
{
    nvme_completion_t cpl;
    ushort_t status;
    ushort_t sq_head;
    int count = 0;

    while (1) {
        NVME_RD(soft, NVME_REG_CSTS); // make PCI bridge complete all DMA write transactions    
        nvme_read_completion(&cpl, q);

        /* Check phase bit */
        if (((cpl.dw3 >> 16) & 1) != ((q->cq_head >> q->size_shift) & 1))
            break;  /* No more completions */

        /* Extract SQ Head from completion (dw2 bits 15:0) */
        sq_head = cpl.dw2 & 0xFFFF;
        if (sq_head >= q->size) {
#ifdef NVME_DBG
            cmn_err(CE_WARN, "nvme_process_completions: weird SQ_HEAD %d it should wraparound",
                    sq_head);
#endif
        }
        q->sq_head = sq_head & q->size_mask;  /* submitters read it, possibly slightly stale */

        /* Process this completion - calls sr_notify with NO locks held */
        status = cpl.dw3 >> 17; // bit 16 is phase
        q->cpl_handler(soft, q, &cpl);
        count++;

#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_process_completions: CID %d, status 0x%x, SQ_HEAD %d",
                cpl.dw3 & 0xFFFF, status, sq_head);
#endif
        /* Advance head */
        q->cq_head++;
    }

    if (count) {
        NVME_WR(soft, q->cq_doorbell, (q->cq_head & q->size_mask));
        pciio_write_gather_flush(soft->pci_vhdl); // make sure these post on IP30
    }

    return count;
}

void
nvme_handle_admin_completion(nvme_soft_t *soft, nvme_queue_t *q, nvme_completion_t *cpl)
{
    ushort_t status_code = (cpl->dw3 >> 17) & 0x7F;
    ushort_t status_type = (cpl->dw3 >> 25) & 0x7;
    ushort_t cid = cpl->dw3 & 0xFFFF;
    nvme_identify_controller_t *id_ctrl;
    nvme_identify_namespace_t *id_ns;
    __uint64_t nsze;
    uint_t lbads;
    int i;

    if (status_code != NVME_SC_SUCCESS) {
        cmn_err(CE_WARN, "nvme_handle_admin_completion: command failed, "
                "CID %d, status type %d, code %d",
                cid, status_type, status_code);
        return;
    }

    /* Handle specific commands based on CID */
    switch (cid) {
    case NVME_ADMIN_CID_IDENTIFY_CONTROLLER:
#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_handle_admin_completion: processing Identify Controller");
#endif
#ifdef HEART_INVALIDATE_WAR
        heart_invalidate_war((caddr_t)soft->utility_buffer, sizeof(NBPP));
#endif
        id_ctrl = (nvme_identify_controller_t *)soft->utility_buffer;

        /* Copy serial number (20 bytes, space-padded ASCII) and null-terminate */
        bcopy(id_ctrl->serial_number, soft->serial, 20);
        soft->serial[20] = '\0';

        /* Copy model number (40 bytes, space-padded ASCII) and null-terminate */
        bcopy(id_ctrl->model_number, soft->model, 40);
        soft->model[40] = '\0';

        /* Copy firmware revision (8 bytes, space-padded ASCII) and null-terminate */
        bcopy(id_ctrl->firmware_revision, soft->firmware_rev, 8);
        soft->firmware_rev[8] = '\0';

        soft->num_namespaces = NVME_MEMRDBS(&id_ctrl->number_of_namespaces);

        /* Get MDTS (Maximum Data Transfer Size) */
        soft->mdts = id_ctrl->mdts;

        /* Calculate maximum transfer size in blocks */
        if (soft->mdts == 0) {
            /* 0 means no limit - cap at something reasonable */
            soft->max_transfer_blocks = 0xFFFF;  /* 32MB with 512-byte blocks */
        } else {
            /* MDTS is 2^n pages, convert to blocks */
            uint_t max_pages = (1 << soft->mdts);
            soft->max_transfer_blocks = (max_pages * (1u << (soft->min_page_size + 12))) / 512;
        }
#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme: Controller - SN=%s, Model=%s, FW=%s, NS=%d",
                soft->serial, soft->model, soft->firmware_rev, soft->num_namespaces);
        cmn_err(CE_NOTE, "nvme: MDTS=%d (max transfer = %d blocks = %d KB)",
                soft->mdts, soft->max_transfer_blocks,
                (soft->max_transfer_blocks * 512) / 1024);
#endif
        break;

    case NVME_ADMIN_CID_IDENTIFY_NAMESPACE: {
        uint_t flbas;

#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_handle_admin_completion: processing Identify Namespace");
#endif
#ifdef IP30
        //heart_dcache_inval((caddr_t)soft->utility_buffer, sizeof(NBPP));
        heart_invalidate_war((caddr_t)soft->utility_buffer, sizeof(NBPP));
#endif
        id_ns = (nvme_identify_namespace_t *)soft->utility_buffer;

        /* Get namespace size (NSZE) - 64-bit value, little-endian */
        nsze = (__uint64_t)NVME_MEMRDBS(&id_ns->nsze_lo) |
               (((__uint64_t)NVME_MEMRDBS(&id_ns->nsze_hi)) << 32);

        /* Get formatted LBA size (FLBAS) - bits 23:16 of features_nlbaf_flbas_mc */
        flbas = (NVME_MEMRDBS(&id_ns->features_nlbaf_flbas_mc) >> 16) & 0xF;

        /* Get LBA data size (LBADS) from LBA format - bits 23:16 of lba_formats[flbas].dw0 */
        lbads = (NVME_MEMRDBS(&id_ns->lba_formats[flbas].dw0) >> 16) & 0xFF;

        soft->num_blocks = nsze;
        soft->block_size = 1u << lbads;  /* 2^LBADS */
        soft->lba_shift = lbads;
        soft->nsid = 1;  /* We always use namespace 1 */

#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme: Namespace 1 - Size=%llu blocks, Block size=%u bytes (2^%u)",
                soft->num_blocks,
                soft->block_size,
                soft->lba_shift);
#endif
        break;
    }

    case NVME_ADMIN_CID_CREATE_CQ:
#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_handle_admin_completion: I/O Completion Queue created");
#endif
        break;

    case NVME_ADMIN_CID_CREATE_SQ:
#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_handle_admin_completion: I/O Submission Queue created");
#endif
        break;

    default:
#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_handle_admin_completion: command CID %d completed", cid);
#endif
        break;
    }
}

/*
 * Helper: Map NVMe status to SCSI sense key and ASC/ASCQ
 */
static void
nvme_map_status_to_sense(scsi_request_t *req, ushort_t status_type, ushort_t status_code)
{
    u_char sense_key = 0x0B;  /* Default: ABORTED COMMAND */
    u_char asc = 0x00;
    u_char ascq = status_code;  /* Use NVMe code as ASCQ */

    switch (status_type) {
    case 0:  /* Generic Command Status */
        switch (status_code) {
        case NVME_SC_INVALID_OPCODE:
        case NVME_SC_INVALID_FIELD:
        case NVME_SC_INVALID_NS:
            sense_key = 0x05;  /* ILLEGAL REQUEST */
            asc = 0x20;        /* Invalid command */
            break;
        case NVME_SC_DATA_XFER_ERROR:
        case NVME_SC_INTERNAL:
            sense_key = 0x04;  /* HARDWARE ERROR */
            asc = 0x44;        /* Internal target failure */
            break;
        case NVME_SC_LBA_RANGE:
            sense_key = 0x05;  /* ILLEGAL REQUEST */
            asc = 0x21;        /* LBA out of range */
            break;
        default:
            sense_key = 0x0B;  /* ABORTED COMMAND */
            asc = 0x00;
            break;
        }
        break;

    case 1:  /* Command Specific Status */
        sense_key = 0x0B;  /* ABORTED COMMAND */
        asc = 0x00;
        break;

    case 2:  /* Media Errors */
        sense_key = 0x03;  /* MEDIUM ERROR */
        asc = 0x11;        /* Unrecovered read error */
        break;

    default:
        sense_key = 0x0B;  /* ABORTED COMMAND */
        asc = 0x00;
        break;
    }

    /* Build sense data */
    if (req->sr_sense && req->sr_senselen >= 18) {
        bzero(req->sr_sense, req->sr_senselen);
        req->sr_sense[0] = 0x70;      /* Current error, fixed format */
        req->sr_sense[2] = sense_key;
        req->sr_sense[7] = 10;        /* Additional sense length */
        req->sr_sense[12] = asc;
        req->sr_sense[13] = ascq;
        req->sr_sensegotten = 18;
    } else {
        req->sr_sensegotten = 0;
    }

    req->sr_status = SC_GOOD;
    req->sr_scsi_status = ST_CHECK;
    req->sr_resid = req->sr_buflen;  /* No data transferred on error */
}

/*
 * nvme_handle_io_completion: Handle I/O command completion
 *
 * Called from interrupt handler when an I/O command completes.
 * Retrieves the SCSI request, sets completion status, frees resources,
 * and notifies the SCSI layer.
 */
void
nvme_handle_io_completion(nvme_soft_t *soft, nvme_queue_t *q, nvme_completion_t *cpl)
{
    ushort_t status_code = (cpl->dw3 >> 17) & 0x7F;
    ushort_t status_type = (cpl->dw3 >> 25) & 0x7;
    ushort_t cid = cpl->dw3 & 0xFFFF;
    scsi_request_t *req;
    nvme_cmd_info_t *cmd_info;

    /* Check if this is a special CID (not in normal CID range) */
    if (cid == NVME_IO_CID_FLUSH) {
        /* Special flush completion - not tied to any scsi_request */
#ifdef NVME_DBG
        if (status_code == NVME_SC_SUCCESS) {
            cmn_err(CE_NOTE, "nvme_handle_io_completion: special flush (CID 0x%x) completed successfully", cid);
        } else {
            cmn_err(CE_WARN, "nvme_handle_io_completion: special flush (CID 0x%x) failed, "
                    "status type %d, code %d", cid, status_type, status_code);
        }
#endif
        return;
    }

    /* Look up the SCSI request for this CID, this also frees the slot and PRPs. */
    req = nvme_io_cid_done(soft, cid);
    if (!req) {
#ifdef NVME_DBG        
        cmn_err(CE_WARN, "nvme_handle_io_completion: spurious completion for CID %d", cid);
#endif        
        return;
    }

#if 0
    if (req->sr_flags & SRF_DIR_IN)
    {
        if (req->sr_flags & SRF_MAPBP)
        {
            bp_heart_invalidate_war(req->sr_bp);
        }
        else
        {
#ifdef HEART_INVALIDATE_WAR
            heart_invalidate_war(req->sr_buffer, req->sr_buflen);
#else
            if (req->sr_flags & SRF_FLUSH) {
#ifdef HEART_COHERENCY_WAR
#endif
            }
#endif            
        }
    }
#endif

    /* Process completion status */
    if (status_code == NVME_SC_SUCCESS) {
        nvme_set_success(req);
#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_handle_io_completion: CID %d completed successfully", cid);
#endif
    } else {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_handle_io_completion: CID %d failed, "
                "status type %d, code %d", cid, status_type, status_code);
#endif
        nvme_map_status_to_sense(req, status_type, status_code);
    }

    /* Notify SCSI layer - this completes the request */
    if (req->sr_notify) {
        req->sr_ha = NULL;
        (*req->sr_notify)(req);
    }
}

#ifdef NVME_TEST
void
nvme_test_admin_completion(nvme_soft_t *soft, nvme_queue_t *q, nvme_completion_t *cpl)
{
    ushort_t status_code = (cpl->dw3 >> 17) & 0x7F;
    ushort_t status_type = (cpl->dw3 >> 25) & 0x7;
    ushort_t cid = cpl->dw3 & 0xFFFF;
    int i;

    if (status_code != NVME_SC_SUCCESS) {
        cmn_err(CE_WARN, "nvme_test_admin_completion: command failed, "
                "CID %d, status type %d, code %d",
                cid, status_type, status_code);
        soft->test_cid = (cid ^ 0xFFFF);
        return;
    }

    soft->test_cid = cid;
}

void
nvme_test_io_completion(nvme_soft_t *soft, nvme_queue_t *q, nvme_completion_t *cpl)
{
    ushort_t status_code = (cpl->dw3 >> 17) & 0x7F;
    ushort_t status_type = (cpl->dw3 >> 25) & 0x7;
    ushort_t cid = cpl->dw3 & 0xFFFF;
    int i;

    if (status_code != NVME_SC_SUCCESS) {
        cmn_err(CE_WARN, "nvme_test_io_completion: command failed, "
                "CID %d, status type %d, code %d",
                cid, status_type, status_code);
        soft->test_cid = (cid ^ 0xFFFF);
        return;
    }

    soft->test_cid = cid;
}
#endif
