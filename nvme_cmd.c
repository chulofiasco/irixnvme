/*
 * nvme_cmd.c - NVMe Command Construction and Submission
 *
 * Functions for building and submitting NVMe commands to the controller.
 */

#include "nvmedrv.h"

/*
 * nvme_submit_cmd: Submit a command to a queue
 *
 * Returns:
 *   0 on success
 *   -1 if queue is full
 */
int
nvme_submit_cmd(nvme_soft_t *soft, nvme_queue_t *q, nvme_command_t *cmd)
{
    uint_t next_tail;
    nvme_command_t *sq_entry;

    mutex_lock(&q->lock, PZERO);

    /* Calculate next tail position */
    next_tail = (q->sq_tail + 1) & q->size_mask;

    /* Check if queue is full - we can't let tail catch up to head */
    if (next_tail == q->sq_head) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_submit_cmd: queue %d is full (head=%d, tail=%d)",
                q->qid, q->sq_head, q->sq_tail);
#endif
        mutex_unlock(&q->lock);
        return -1;
    }

    sq_entry = &q->sq[q->sq_tail];
#ifdef NVME_DBG_CMD
    cmn_err(CE_NOTE, "nvme_submit_cmd: Writing to SQ[%u] at %p", q->sq_tail, sq_entry);
#endif
    /* Write command to submission queue entry */
    NVME_MEMWR(&sq_entry->cdw0, cmd->cdw0);
    NVME_MEMWR(&sq_entry->nsid, cmd->nsid);
    NVME_MEMWR(&sq_entry->cdw2, cmd->cdw2);
    NVME_MEMWR(&sq_entry->cdw3, cmd->cdw3);
    NVME_MEMWR(&sq_entry->mptr_lo, cmd->mptr_lo);
    NVME_MEMWR(&sq_entry->mptr_hi, cmd->mptr_hi);
    NVME_MEMWR(&sq_entry->prp1_lo, cmd->prp1_lo);
    NVME_MEMWR(&sq_entry->prp1_hi, cmd->prp1_hi);
    NVME_MEMWR(&sq_entry->prp2_lo, cmd->prp2_lo);
    NVME_MEMWR(&sq_entry->prp2_hi, cmd->prp2_hi);
    NVME_MEMWR(&sq_entry->cdw10, cmd->cdw10);
    NVME_MEMWR(&sq_entry->cdw11, cmd->cdw11);
    NVME_MEMWR(&sq_entry->cdw12, cmd->cdw12);
    NVME_MEMWR(&sq_entry->cdw13, cmd->cdw13);
    NVME_MEMWR(&sq_entry->cdw14, cmd->cdw14);
    NVME_MEMWR(&sq_entry->cdw15, cmd->cdw15);
#ifdef IP30
    heart_dcache_wb_inval((caddr_t)sq_entry, sizeof(nvme_command_t));
#endif

#ifdef NVME_DBG_CMD
    /* Dump what we just wrote to the SQ */
    nvme_dump_sq_entry(sq_entry, "After writing to SQ");
#endif /* NVME_DBG_CMD */
    /* Advance tail */
    q->sq_tail = next_tail;

#ifdef NVME_DBG_EXTRA
    cmn_err(CE_NOTE, "nvme_submit_cmd: Ringing doorbell at offset 0x%x with value %u",
            q->sq_doorbell, q->sq_tail);
#endif
    /* Ring doorbell to notify controller */
    NVME_WR(soft, q->sq_doorbell, q->sq_tail);
    pciio_write_gather_flush(soft->pci_vhdl); // make sure these post on IP30

#ifdef NVME_DBG_EXTRA
    /* Verify the doorbell was written */
    cmn_err(CE_NOTE, "nvme_submit_cmd: Doorbell readback = 0x%08x",
            NVME_RD(soft, q->sq_doorbell));
#endif
    mutex_unlock(&q->lock);
#ifdef NVME_COMPLETION_THREAD
    /* If interrupts are disabled, wake polling thread to check for completions */
    if (!soft->interrupts_enabled) {
        nvme_kick_poll_thread(soft);
    }
#endif
#ifdef NVME_COMPLETION_MANUAL
    /* Wait a bit and check if completion arrived (in case interrupt isn't working) */
    

    /* Manually check for completions */
    {
        uint_t old_head = q->cq_head;
        int num_processed = 0;
        int np;
        while ((num_processed += nvme_process_completions(soft, q)) == 0) {
                us_delay(1000); /* 1 millisecond */
        }
        while ((np = nvme_process_completions(soft, q)) > 0) {
                num_processed += np;
                us_delay(1000); /* 1 millisecond */
        }

        cmn_err(CE_WARN, "nvme_scsi_read_write: after 1ms delay, manually processed %d completions (cq_head %d->%d)  int count=%d",
                num_processed, old_head, q->cq_head, nvme_intcount);
    }
#endif

    return 0;
}

/*
 * nvme_admin_identify_controller: Send Identify Controller command
 *
 * Uses utility buffer to retrieve controller identification data.
 * Stores serial, model, firmware, and number of namespaces in soft state.
 */
int
nvme_admin_identify_controller(nvme_soft_t *soft)
{
    nvme_command_t cmd;
    nvme_identify_controller_t *id_ctrl;

#ifdef NVME_DBG_CMD
    cmn_err(CE_NOTE, "nvme_admin_identify_controller: sending command");
#endif
    /* Clear utility buffer */
    bzero(soft->utility_buffer, NBPP);
#ifdef IP30
    heart_dcache_wb_inval((caddr_t)soft->utility_buffer, sizeof(NBPP));
#else
    dki_dcache_wbinval((caddr_t)soft->utility_buffer, sizeof(NBPP));
#endif

    /* Build Identify Controller command */
    bzero(&cmd, sizeof(cmd));

    /* CDW0: Opcode (7:0), Flags (15:8), CID (31:16) */
    cmd.cdw0 = NVME_ADMIN_IDENTIFY | (NVME_ADMIN_CID_IDENTIFY_CONTROLLER << 16);

    /* NSID: not used for controller identify */
    cmd.nsid = 0;

    /* PRP1: physical address of utility buffer (controller data destination) */
    cmd.prp1_lo = PHYS64_LO(soft->utility_buffer_phys);
    cmd.prp1_hi = PHYS64_HI(soft->utility_buffer_phys);

    /* PRP2: not needed (data is only 4KB) */
    cmd.prp2_lo = 0;
    cmd.prp2_hi = 0;

    /* CDW10: CNS = 0x01 for Identify Controller */
    cmd.cdw10 = NVME_CNS_CONTROLLER;

#ifdef NVME_DBG_CMD
    cmn_err(CE_NOTE, "nvme_admin_identify_controller: Built command:");
    cmn_err(CE_NOTE, "  cdw0=0x%08x (opcode=0x%02x, cid=0x%04x)",
            cmd.cdw0, cmd.cdw0 & 0xFF, (cmd.cdw0 >> 16) & 0xFFFF);
    cmn_err(CE_NOTE, "  nsid=0x%08x", cmd.nsid);
    cmn_err(CE_NOTE, "  prp1=0x%08x%08x (virt=%p, phys=0x%llx)",
            cmd.prp1_hi, cmd.prp1_lo,
            soft->utility_buffer, soft->utility_buffer_phys);
    cmn_err(CE_NOTE, "  cdw10=0x%08x (CNS)", cmd.cdw10);
#endif
    /* Submit command */
    if (nvme_submit_cmd(soft, &soft->admin_queue, &cmd) != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_admin_identify_controller: failed to submit command (queue full?)");
#endif
        return 0;  /* Failure */
    }
#ifdef NVME_DBG_CMD
    cmn_err(CE_NOTE, "nvme_admin_identify_controller: command submitted, waiting for completion");
#endif
    return 1;  /* Success - command submitted */
}

/*
 * nvme_admin_identify_namespace: Send Identify Namespace command
 *
 * Uses utility buffer to retrieve namespace identification data for namespace 1.
 * Stores namespace size and block size in soft state for later use by SCSI emulation.
 */
int
nvme_admin_identify_namespace(nvme_soft_t *soft)
{
    nvme_command_t cmd;

#ifdef NVME_DBG_CMD
    cmn_err(CE_NOTE, "nvme_admin_identify_namespace: sending command for NSID 1");
#endif
    /* Clear utility buffer */
    bzero(soft->utility_buffer, NBPP);
#ifdef IP30
    heart_dcache_wb_inval((caddr_t)soft->utility_buffer, sizeof(NBPP));
#else
    dki_dcache_wbinval((caddr_t)soft->utility_buffer, sizeof(NBPP));
#endif

    /* Build Identify Namespace command */
    bzero(&cmd, sizeof(cmd));

    /* CDW0: Opcode (7:0), Flags (15:8), CID (31:16) */
    cmd.cdw0 = NVME_ADMIN_IDENTIFY | (NVME_ADMIN_CID_IDENTIFY_NAMESPACE << 16);

    cmd.nsid = 1;

    /* PRP1: physical address of utility buffer (namespace data destination) */
    cmd.prp1_lo = PHYS64_LO(soft->utility_buffer_phys);
    cmd.prp1_hi = PHYS64_HI(soft->utility_buffer_phys);

    /* PRP2: not needed (data is only 4KB) */
    cmd.prp2_lo = 0;
    cmd.prp2_hi = 0;

    /* CDW10: CNS = 0x00 for Identify Namespace */
    cmd.cdw10 = NVME_CNS_NAMESPACE;

    /* Submit command */
    if (nvme_submit_cmd(soft, &soft->admin_queue, &cmd) != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_admin_identify_namespace: failed to submit command (queue full?)");
#endif
        return 0;  /* Failure */
    }

#ifdef NVME_DBG
    cmn_err(CE_NOTE, "nvme_admin_identify_namespace: command submitted, waiting for completion");
#endif
    return 1;  /* Success - command submitted */
}

/*
 * nvme_admin_create_cq: Create I/O Completion Queue
 */
int
nvme_admin_create_cq(nvme_soft_t *soft, ushort_t qid, ushort_t qsize,
                     alenaddr_t phys_addr, ushort_t vector)
{
    nvme_command_t cmd;

    bzero(&cmd, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_CREATE_CQ;
    cmd.cdw0 |= NVME_ADMIN_CID_CREATE_CQ << 16;

    cmd.prp1_lo = PHYS64_LO(phys_addr);
    cmd.prp1_hi = PHYS64_HI(phys_addr);
    cmd.cdw10 = ((qsize - 1) << 16) | qid;
    cmd.cdw11 = NVME_QUEUE_PHYS_CONTIG 
#ifdef NVME_COMPLETION_INTERRUPT
              | NVME_QUEUE_IRQ_ENABLED | (vector << 16)
#endif
            ;

    if (nvme_submit_cmd(soft, &soft->admin_queue, &cmd) != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_admin_create_cq: failed to submit command");
#endif
        return 0;
    }
    return 1;
}

/*
 * nvme_admin_create_sq: Create I/O Submission Queue
 */
int
nvme_admin_create_sq(nvme_soft_t *soft, ushort_t qid, ushort_t qsize,
                     alenaddr_t phys_addr, ushort_t cqid)
{
    nvme_command_t cmd;

    bzero(&cmd, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_CREATE_SQ;
    cmd.cdw0 |= NVME_ADMIN_CID_CREATE_SQ << 16;

    cmd.prp1_lo = PHYS64_LO(phys_addr);
    cmd.prp1_hi = PHYS64_HI(phys_addr);
    cmd.cdw10 = ((qsize - 1) << 16) | qid;
    cmd.cdw11 = NVME_QUEUE_PHYS_CONTIG | (cqid << 16);

    if (nvme_submit_cmd(soft, &soft->admin_queue, &cmd) != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_admin_create_sq: failed to submit command");
#endif
        return 0;
    }
    return 1;
}

/*
 * nvme_admin_delete_sq: Delete I/O Submission Queue
 */
int
nvme_admin_delete_sq(nvme_soft_t *soft, ushort_t qid)
{
    nvme_command_t cmd;

    bzero(&cmd, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_DELETE_SQ;
    cmd.cdw0 |= NVME_ADMIN_CID_DELETE_SQ << 16;
    cmd.cdw10 = qid;

    if (nvme_submit_cmd(soft, &soft->admin_queue, &cmd) != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_admin_delete_sq: failed to submit command");
#endif
        return 0;
    }
    return 1;
}

/*
 * nvme_admin_delete_cq: Delete I/O Completion Queue
 */
int
nvme_admin_delete_cq(nvme_soft_t *soft, ushort_t qid)
{
    nvme_command_t cmd;

    bzero(&cmd, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_DELETE_CQ;
    cmd.cdw0 |= NVME_ADMIN_CID_DELETE_CQ << 16;
    cmd.cdw10 = qid;

    if (nvme_submit_cmd(soft, &soft->admin_queue, &cmd) != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_admin_delete_cq: failed to submit command");
#endif
        return 0;
    }
    return 1;
}

/*
 * nvme_io_build_rw_command: Build NVMe Read/Write command from SCSI request
 *
 * Translates SCSI READ/WRITE commands (READ6, READ10, READ16, WRITE6, WRITE10, WRITE16)
 * into NVMe Read/Write commands. Parses the SCSI CDB and fills in the NVMe command
 * structure with opcode, namespace ID, LBA, and block count.
 *
 * For multi-command transfers (cmd_index > 0), the LBA and block count are adjusted
 * based on the command index and max_transfer_blocks.
 *
 * PRP entries are NOT set by this function - they must be filled in separately
 * by calling nvme_build_prps_from_alenlist().
 *
 * Arguments:
 *   soft      - Controller state
 *   req       - SCSI request structure
 *   cmd       - NVMe command structure to fill in (opcode, nsid, LBA, block count)
 *   cmd_index - Index of this command in multi-command sequence (0-based)
 *
 * Returns:
 *   1 on success
 *   0 on failure (unsupported CDB opcode, caller set sense data)
 *  -1 on failure (we set sense data)
 */
int
nvme_io_build_rw_command(nvme_soft_t *soft, scsi_request_t *req,
                         nvme_command_t *cmd, unsigned int cmd_index)
{
    u_char *cdb = req->sr_command;
    __uint64_t lba = 0;
    uint_t num_blocks = 0;
    int is_write = 0;
    uint_t cdb_opcode;

    /* Clear command structure */
    bzero(cmd, sizeof(*cmd));

    /* Parse CDB based on opcode */
    cdb_opcode = cdb[0];

    switch (cdb_opcode) {
    case SCSIOP_READ_6:
    case SCSIOP_WRITE_6:
        /* READ(6)/WRITE(6) format:
         * Byte 0: Opcode
         * Byte 1: LBA bits 20-16 (5 bits) in bits 4-0
         * Byte 2: LBA bits 15-8
         * Byte 3: LBA bits 7-0
         * Byte 4: Transfer length (0 = 256 blocks)
         * Byte 5: Control
         */
        lba = ((__uint64_t)(cdb[1] & 0x1F) << 16) |
              ((__uint64_t)cdb[2] << 8) |
              ((__uint64_t)cdb[3]);
        num_blocks = cdb[4];
        if (num_blocks == 0) {
            num_blocks = 256;  /* 0 means 256 blocks in READ(6)/WRITE(6) */
        }
        is_write = (cdb_opcode == SCSIOP_WRITE_6);
        break;

    case SCSIOP_READ_10:
    case SCSIOP_WRITE_10:
        /* READ(10)/WRITE(10) format:
         * Byte 0: Opcode
         * Byte 1: Flags
         * Byte 2: LBA bits 31-24
         * Byte 3: LBA bits 23-16
         * Byte 4: LBA bits 15-8
         * Byte 5: LBA bits 7-0
         * Byte 6: Reserved
         * Byte 7: Transfer length bits 15-8
         * Byte 8: Transfer length bits 7-0
         * Byte 9: Control
         */
        lba = ((__uint64_t)cdb[2] << 24) |
              ((__uint64_t)cdb[3] << 16) |
              ((__uint64_t)cdb[4] << 8) |
              ((__uint64_t)cdb[5]);
        num_blocks = ((uint_t)cdb[7] << 8) | ((uint_t)cdb[8]);
        is_write = (cdb_opcode == SCSIOP_WRITE_10);
        break;

    case SCSIOP_READ_16:
    case SCSIOP_WRITE_16:
        /* READ(16)/WRITE(16) format:
         * Byte 0: Opcode
         * Byte 1: Flags
         * Byte 2: LBA bits 63-56
         * Byte 3: LBA bits 55-48
         * Byte 4: LBA bits 47-40
         * Byte 5: LBA bits 39-32
         * Byte 6: LBA bits 31-24
         * Byte 7: LBA bits 23-16
         * Byte 8: LBA bits 15-8
         * Byte 9: LBA bits 7-0
         * Byte 10: Transfer length bits 31-24
         * Byte 11: Transfer length bits 23-16
         * Byte 12: Transfer length bits 15-8
         * Byte 13: Transfer length bits 7-0
         * Byte 14: Flags
         * Byte 15: Control
         */
        lba = ((__uint64_t)cdb[2] << 56) |
              ((__uint64_t)cdb[3] << 48) |
              ((__uint64_t)cdb[4] << 40) |
              ((__uint64_t)cdb[5] << 32) |
              ((__uint64_t)cdb[6] << 24) |
              ((__uint64_t)cdb[7] << 16) |
              ((__uint64_t)cdb[8] << 8) |
              ((__uint64_t)cdb[9]);
        num_blocks = ((uint_t)cdb[10] << 24) |
                     ((uint_t)cdb[11] << 16) |
                     ((uint_t)cdb[12] << 8) |
                     ((uint_t)cdb[13]);
        is_write = (cdb_opcode == SCSIOP_WRITE_16);
        break;

    default:
        cmn_err(CE_WARN, "nvme_io_build_rw_command: unsupported CDB opcode 0x%02x",
                cdb_opcode);
        return 0;
    }

    /* Adjust LBA and block count based on command index for multi-command transfers */
    lba += cmd_index * soft->max_transfer_blocks;
    num_blocks -= cmd_index * soft->max_transfer_blocks;
    if (num_blocks > soft->max_transfer_blocks) {
        num_blocks = soft->max_transfer_blocks;
    }

    /* Build NVMe command header - CID will be set by caller */
    if (is_write) {
        cmd->cdw0 = NVME_CMD_WRITE;
    } else {
        cmd->cdw0 = NVME_CMD_READ;
    }

    /* Set namespace ID (hardcoded to 1) */
    cmd->nsid = 1;

    /* Set LBA (CDW10 = lower 32 bits, CDW11 = upper 32 bits) */
    cmd->cdw10 = (__uint32_t)(lba & 0xFFFFFFFF);
    cmd->cdw11 = (__uint32_t)(lba >> 32);

    /* Set number of logical blocks (0-based, so subtract 1) */
    cmd->cdw12 = (num_blocks > 0) ? (num_blocks - 1) : 0;

    /* Remaining fields already zeroed by bzero() above */

#ifdef NVME_DBG_CMD
    cmn_err(CE_NOTE, "nvme_io_build_rw_command: %s cmd_index=%u LBA=%llu blocks=%u",
            is_write ? "WRITE" : "READ", cmd_index, lba, num_blocks);
#endif
    return 1;
}

/*
 * nvme_get_translated_addr: Get and translate next page from alenlist
 *
 * This helper combines alenlist_get with pciio_dmatrans_addr to fetch
 * the next page from an alenlist and translate it to a PCI bus address.
 *
 * Arguments:
 *   soft         - Controller state (for pci_vhdl)
 *   alenlist     - Alenlist to fetch from (cursor must be initialized)
 *   maxlength    - Maximum bytes to fetch (typically nvme_page_size)
 *   out_address  - Output: Translated PCI bus address (physical address)
 *   out_length   - Output: Length of this segment in bytes
 *   is_write     - whether the operatio is read or write
 *
 * Returns:
 *   0 on success
 *   -1 on failure (alenlist exhausted or DMA translation failed)
 */
int
nvme_get_translated_addr(nvme_soft_t *soft, alenlist_t alenlist, size_t maxlength,
                        alenaddr_t *out_address, size_t *out_length, int is_write)
{
    alenaddr_t address;
    size_t length;

    /* Get next entry from alenlist */
    if (alenlist_get(alenlist, NULL, maxlength, &address, &length, 0) != ALENLIST_SUCCESS) {
        return -1;
    }

    /* Translate to PCI bus address with explicit cast to quiet warnings */
    address = pciio_dmatrans_addr(soft->pci_vhdl, NULL, (paddr_t)address, length,
                                  PCIIO_DMA_DATA | DMATRANS64 | PCIIO_BYTE_STREAM
#ifdef NVME_VCHAN1
                                  | PCIBR_VCHAN1
#endif
#ifdef NVME_READ_BARRIER
                                  | (is_write ? 0 : PCIBR_BARRIER)
#endif
#ifdef IP30
                                  | (is_write ? 0 : PCIIO_NOPREFETCH)
#endif
                                );
    if (!address) {
        return -1;
    }

    *out_address = address;
    *out_length = length;
    return 0;
}

/*
 * nvme_prepare_alenlist: Prepare alenlist from SCSI request for PRP building
 *
 * This function handles the complexity of converting a SCSI request with various
 * data buffer formats into an alenlist that can be walked to build PRPs.
 *
 * Handles multiple data buffer modes:
 * - SRF_ALENLIST: User virtual address with alenlist in bp->b_private
 * - SRF_MAPBP: Buffer pointer that needs conversion via buf_to_alenlist
 * - SRF_MAP/SRF_MAPUSER: Kernel virtual address (with alignment checks for USERMAP)
 *
 * Arguments:
 *   soft            - Controller state
 *   req             - SCSI request to convert
 *   out_alenlist    - Output: alenlist to use for PRP building
 *   out_need_unlock - Output: 1 if caller must unlock soft->alenlist_lock when done
 *
 * Returns:
 *   0 on success
 *   -1 on failure (logs error via cmn_err)
 */
int
nvme_prepare_alenlist(nvme_soft_t *soft, scsi_request_t *req,
                      alenlist_t *out_alenlist, int *out_need_unlock)
{
    alenlist_t alenlist = NULL;
    int need_unlock = 0;

    /* If no data transfer, nothing to prepare */
    if (req->sr_buflen == 0 || req->sr_buffer == NULL) {
        *out_alenlist = NULL;
        *out_need_unlock = 0;
        return 0;
    }

    /*
     * Determine data buffer source and build/extract alenlist
     * Use pre-allocated alenlist for MAPBP/MAP, use provided one for ALENLIST
     */
    if (req->sr_flags & SRF_ALENLIST) {
        /*
         * User virtual address case - alenlist already created by upper layer
         * and stored in the buffer's b_private field. Use it directly.
         */
        if (!IS_KUSEG(req->sr_buffer)) {
#ifdef NVME_DBG
            cmn_err(CE_WARN, "nvme_prepare_alenlist: SRF_ALENLIST but address not KUSEG");
#endif
            return -1;
        }

        alenlist = (alenlist_t)(((buf_t *)(req->sr_bp))->b_private);
        if (!alenlist) {
#ifdef NVME_DBG
            cmn_err(CE_WARN, "nvme_prepare_alenlist: SRF_ALENLIST but no alenlist in bp->b_private");
#endif
            return -1;
        }

#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_prepare_alenlist: using user alenlist from bp->b_private");
#endif
        /* Don't lock/unlock - we don't own this alenlist */
    } else {
        /*
         * For MAPBP/MAP/MAPUSER: use pre-allocated alenlist (avoids dynamic allocation failures)
         * Lock it to prevent concurrent use, following ql.c pattern
         */
        mutex_lock(&soft->alenlist_lock, PZERO);
        need_unlock = 1;
        alenlist = soft->alenlist;

        if (req->sr_flags & SRF_MAPBP) {
            /*
             * Buffer-based mapping case - convert buf_t to alenlist
             */
            if (BP_ISMAPPED(((buf_t *)(req->sr_bp)))) {
#ifdef NVME_DBG
                cmn_err(CE_WARN, "nvme_prepare_alenlist: SRF_MAPBP but buffer is already mapped");
#endif
                mutex_unlock(&soft->alenlist_lock);
                return -1;
            }

            /*
             * Cache flush for buf_t - always use bp_dcache_wbinval for buf_t
             * This handles both DMA read and write cases properly
             * If upper layer tells us to flush we flush, use war version first 
             * We have to use normal version afterwards because war version reads back!
             */
            if (req->sr_flags & SRF_FLUSH) {
#ifdef HEART_INVALIDATE_WAR
                if (req->sr_flags & SRF_DIR_IN) {
                    /* is this one appropriate for writes? it does not seem so becauuse it doesnt do writeback!. so lets call it only for reads. */
                    bp_heart_invalidate_war((buf_t *)(req->sr_bp));
                } else
#endif
                bp_dcache_wbinval((buf_t *)(req->sr_bp));
            }

            /* Convert buf_t to alenlist (buf_to_alenlist clears the alenlist first) */
            if (buf_to_alenlist(alenlist, (buf_t *)(req->sr_bp), AL_NOCOMPACT) == NULL) {
#ifdef NVME_DBG
                cmn_err(CE_WARN, "nvme_prepare_alenlist: buf_to_alenlist failed");
#endif
                mutex_unlock(&soft->alenlist_lock);
                return -1;
            }

#ifdef NVME_DBG
            cmn_err(CE_NOTE, "nvme_prepare_alenlist: converted buf_t to alenlist (SRF_MAPBP)");
#endif
        } else if (req->sr_flags & (SRF_MAP | SRF_MAPUSER)) {
            /*
             * Virtual address case - convert to alenlist
             * Use IS_KUSEG() to determine actual address type
             */
            int is_user_addr = IS_KUSEG(req->sr_buffer);
#ifdef NVME_DBG
            cmn_err(CE_NOTE, "nvme_prepare_alenlist: MAP flags:0x%02X is_user:%d", req->sr_flags, is_user_addr);
#endif
            /* Verify dword (4-byte) alignment - required for DMA */
            if (((__psunsigned_t)req->sr_buffer & 0x3) != 0) {
#ifdef NVME_DBG
                cmn_err(CE_WARN, "nvme_prepare_alenlist: buffer not dword-aligned (addr=0x%lx)",
                        (__psunsigned_t)req->sr_buffer);
#endif
                mutex_unlock(&soft->alenlist_lock);
                return -1;
            }
            if ((req->sr_buflen & 0x3) != 0) {
#ifdef NVME_DBG
                cmn_err(CE_WARN, "nvme_prepare_alenlist: length not dword-aligned (len=%u)",
                        req->sr_buflen);
#endif
                mutex_unlock(&soft->alenlist_lock);
                return -1;
            }

            /*
             * Cache flush - direction determines the cache operation
             * also in case workaround is needed use the workaround version firs
             * but follow with normal version because workaround rereads the cache lines
             */
            if (req->sr_flags & SRF_FLUSH) {
                if (req->sr_flags & SRF_DIR_IN) {
#ifdef HEART_INVALIDATE_WAR
                    heart_invalidate_war(req->sr_buffer, req->sr_buflen);
#endif
                    dki_dcache_inval(req->sr_buffer, req->sr_buflen);
                } else {
                    dki_dcache_wbinval(req->sr_buffer, req->sr_buflen);
                }
            }

            /* Convert to alenlist based on address type */
            if (is_user_addr) {
                /* User virtual address - use uvaddr_to_alenlist with pre-allocated alenlist */
                if (uvaddr_to_alenlist(alenlist, (uvaddr_t)req->sr_buffer,
                                      req->sr_buflen, 0) == NULL) {
#ifdef NVME_DBG
                    cmn_err(CE_WARN, "nvme_prepare_alenlist: uvaddr_to_alenlist failed");
#endif
                    mutex_unlock(&soft->alenlist_lock);
                    return -1;
                }
#ifdef NVME_DBG
                cmn_err(CE_NOTE, "nvme_prepare_alenlist: converted uvaddr to alenlist (KUSEG)");
#endif
            } else {
                /* Kernel virtual address - use kvaddr_to_alenlist with pre-allocated alenlist */
                if (kvaddr_to_alenlist(alenlist, (caddr_t)req->sr_buffer,
                                      req->sr_buflen, AL_NOCOMPACT) == NULL) {
#ifdef NVME_DBG
                    cmn_err(CE_WARN, "nvme_prepare_alenlist: kvaddr_to_alenlist failed");
#endif
                    mutex_unlock(&soft->alenlist_lock);
                    return -1;
                }
#ifdef NVME_DBG
                cmn_err(CE_NOTE, "nvme_prepare_alenlist: converted kvaddr to alenlist (!KUSEG)");
#endif
            }
        } else {
            /*
             * Unknown or unsupported buffer mode
             * One of SRF_ALENLIST, SRF_MAPBP, SRF_MAP, or SRF_MAPUSER must be set
             */
#ifdef NVME_DBG
            cmn_err(CE_WARN, "nvme_prepare_alenlist: no valid buffer mapping flag set (sr_flags=0x%x)",
                    req->sr_flags);
#endif
            mutex_unlock(&soft->alenlist_lock);
            return -1;
        }
    }

    *out_alenlist = alenlist;
    *out_need_unlock = need_unlock;

    /* Initialize alenlist cursor at offset 0 (cursor will track offset as we walk) */
    if (alenlist != NULL) {
        alenlist_cursor_init(alenlist, 0, NULL);
    }

    return 0;
}

/*
 * nvme_cleanup_alenlist: Clean up alenlist resources
 *
 * Unlocks the alenlist lock if it was locked during nvme_prepare_alenlist().
 *
 * Arguments:
 *   soft        - Controller state
 *   need_unlock - If 1, unlock soft->alenlist_lock
 */
void
nvme_cleanup_alenlist(nvme_soft_t *soft, int need_unlock)
{
    if (need_unlock) {
        mutex_unlock(&soft->alenlist_lock);
    }
}

/*
 * nvme_build_prps_from_alenlist: Build PRP entries from prepared alenlist
 *
 * Walks an alenlist and builds PRP entries for an NVMe command. The alenlist cursor
 * maintains the offset automatically, so calling this multiple times for different
 * cmd_index values will walk through consecutive chunks of the buffer.
 *
 * PRP construction:
 * - Single page: PRP1 only
 * - Dual page: PRP1 + PRP2 as direct addresses
 * - Multi-page: PRP1 + PRP2 pointing to PRP list
 *
 * Arguments:
 *   soft      - Controller state
 *   req       - SCSI request (for buflen and setting status on error)
 *   cmd       - NVMe command to fill in PRP fields (must have cdw12 set)
 *   alenlist  - Prepared alenlist to walk (cursor maintains offset)
 *   cmd_index - Command index in multi-command sequence
 *   is_write
 *
 * Returns:
 *   1 on success
 *   0 on hard error (alenlist translation failure - caller should set error)
 *  -1 on resource exhaustion (BUSY status set internally, caller should retry)
 */
int
nvme_build_prps_from_alenlist(nvme_soft_t *soft, scsi_request_t *req,
                               nvme_command_t *cmd, alenlist_t alenlist,
                               unsigned int cmd_index,
                               int is_write)
{
    alenaddr_t address;
    size_t length;
    size_t fetch_size;
    uint_t chunk_size;
    unsigned int cid = (cmd->cdw0 >> 16) & 0xFFFF;

    /* Clear PRP fields in command */
    cmd->prp1_lo = 0;
    cmd->prp1_hi = 0;
    cmd->prp2_lo = 0;
    cmd->prp2_hi = 0;

    /* If no data transfer, we're done */
    if (alenlist == NULL || req->sr_buflen == 0) {
        return 1;  /* Success - no PRPs needed */
    }

    /* Calculate chunk size for this command */
    chunk_size = req->sr_buflen - (cmd_index * soft->max_transfer_blocks * soft->block_size);
    if (chunk_size > soft->max_transfer_blocks * soft->block_size) {
        chunk_size = soft->max_transfer_blocks * soft->block_size;
    }

#ifdef NVME_DBG_CMD
    cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: cmd_index=%u chunk_size=%u buflen=%u",
            cmd_index, chunk_size, req->sr_buflen);
#endif

    /* Get and translate the first page (possibly partial) for PRP1 */
    fetch_size = (chunk_size < soft->nvme_page_size) ? chunk_size : soft->nvme_page_size;
    if (nvme_get_translated_addr(soft, alenlist, fetch_size, &address, &length, is_write) != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_build_prps_from_alenlist: failed to get/translate first page");
#endif
        return 0;  /* Hard error - DMA translation failed */
    }

    /* Set PRP1 to first address */
    cmd->prp1_lo = PHYS64_LO(address);
    cmd->prp1_hi = PHYS64_HI(address);

#ifdef NVME_DBG
    cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: PRP1=0x%llx len=%u", address, length);
#endif

    /* Subtract what we fetched */
    chunk_size -= length;

    /* Determine if we need PRP2 or a PRP list */
    if (chunk_size == 0) {
        /*
         * CASE 1: Single page transfer - PRP1 only
         */
        cmd->prp2_lo = 0;
        cmd->prp2_hi = 0;

#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: single page (PRP1 only)");
#endif
    } else if (chunk_size <= soft->nvme_page_size) {
        /*
         * CASE 2: Exactly 2 pages - use PRP2 directly (no PRP list needed)
         */
        fetch_size = chunk_size;
        if (nvme_get_translated_addr(soft, alenlist, fetch_size, &address, &length, is_write) != 0) {
#ifdef NVME_DBG
            cmn_err(CE_WARN, "nvme_build_prps_from_alenlist: failed to get/translate second page");
#endif
            return 0;  /* Hard error - DMA translation failed */
        }

        cmd->prp2_lo = PHYS64_LO(address);
        cmd->prp2_hi = PHYS64_HI(address);

#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: dual page (PRP2=0x%llx len=%u)", address, length);
#endif
    } else {
        /*
         * CASE 3: Multi-page transfer - need PRP list(s)
         */
        int num_prp_pages = 0;
        int pool_index;
        void *prp_virt = NULL;
        alenaddr_t prp_phys = 0;
        __uint32_t *prp_list_dwords = NULL;
        uint_t prp_index = soft->nvme_prp_entries-1;  /* Start at max to trigger allocation */

        /* Walk the alenlist page by page to fill PRP list */
        while (chunk_size > 0) {
            /* Get and translate next chunk */
            fetch_size = (chunk_size < soft->nvme_page_size) ? chunk_size : soft->nvme_page_size;
            if (nvme_get_translated_addr(soft, alenlist, fetch_size, &address, &length, is_write) != 0) {
#ifdef NVME_DBG
                cmn_err(CE_WARN, "nvme_build_prps_from_alenlist: failed to get/translate page (remaining=%u)",
                        chunk_size);
#endif
                return 0;  /* Hard error - DMA translation failed */
            }

#ifdef NVME_DBG_EXTRA
            cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: processing page addr=0x%llx len=%u", address, length);
#endif
            /* Check if we need a new PRP list page */
            if (prp_index >= soft->nvme_prp_entries - 1) {
                /* Allocate PRP list page */
                pool_index = nvme_prp_pool_alloc(soft);
                if (pool_index < 0) {
#ifdef NVME_DBG
                    cmn_err(CE_WARN, "nvme_build_prps_from_alenlist: no PRP pool pages available (page %d)",
                            num_prp_pages);
#endif
                    /* Resource exhaustion - set BUSY and return -1 for retry */
                    nvme_set_adapter_status(req, SC_REQUEST, ST_BUSY);
                    return -1;
                }

                /* Store PRP page with CID */
                if (nvme_io_cid_store_prp(soft, cid, pool_index) != 0) {
#ifdef NVME_DBG
                    cmn_err(CE_WARN, "nvme_build_prps_from_alenlist: failed to store PRP index %d with CID %u",
                            pool_index, cid);
#endif
                    nvme_prp_pool_free(soft, pool_index);
                    return 0;
                }

                /* Calculate addresses for PRP list page */
                prp_virt = (void *)((caddr_t)soft->prp_pool + (pool_index * soft->nvme_page_size));
                prp_phys = soft->prp_pool_phys + (pool_index * soft->nvme_page_size);

#ifdef NVME_DBG
                cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: allocated PRP page %d: pool_index=%d, virt=%p, phys=0x%llx",
                        num_prp_pages, pool_index, prp_virt, prp_phys);
#endif
                if (num_prp_pages == 0) {
                    /* First PRP list page - set PRP2 to point to it */
                    cmd->prp2_lo = PHYS64_LO(prp_phys);
                    cmd->prp2_hi = PHYS64_HI(prp_phys);
                } else {
                    /* Subsequent page - chain from previous page's last entry */
                    NVME_MEMWR(&prp_list_dwords[(soft->nvme_prp_entries - 1) * 2], PHYS64_LO(prp_phys));
                    NVME_MEMWR(&prp_list_dwords[(soft->nvme_prp_entries - 1) * 2 + 1], PHYS64_HI(prp_phys));
#ifdef IP30
                    heart_dcache_wb_inval(prp_list_dwords, soft->nvme_prp_entries << 3);
#endif                    
#ifdef NVME_DBG
                    cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: chained page %d -> page %d (phys=0x%llx)",
                            num_prp_pages - 1, num_prp_pages, prp_phys);
#endif
                }

                /* Move to newly allocated page */
                num_prp_pages++;
                prp_index = 0;
                prp_list_dwords = (__uint32_t *)prp_virt;
            }

            NVME_MEMWR(&prp_list_dwords[prp_index * 2], PHYS64_LO(address));
            NVME_MEMWR(&prp_list_dwords[prp_index * 2 + 1], PHYS64_HI(address));

            prp_index++;
            chunk_size -= length;
        }
#ifdef IP30
        heart_dcache_wb_inval(prp_list_dwords, prp_index << 3);
#endif                    

#ifdef NVME_DBG
        cmn_err(CE_NOTE, "nvme_build_prps_from_alenlist: multi-page complete (%d PRP pages, %d entries in last page)",
                num_prp_pages, prp_index);
#endif
    }

    return 1;  /* Success */
}

/*
 * nvme_prp_pool_init: Initialize the PRP list pool
 *
 * Allocates a pool of pages for PRP lists (32 pages = 128KB).
 * Each page can hold up to 512 PRP entries (4096 bytes / 8 bytes per entry).
 *
 * Returns:
 *   0 on success
 *   -1 on failure
 */
int
nvme_prp_pool_init(nvme_soft_t *soft)
{
    int pages;

    /* Allocate pool memory (32 pages = 128KB) */
    pages = NVME_PRP_POOL_SIZE * (NBPP / soft->nvme_page_size);
#ifdef NVME_DBG
    cmn_err(CE_NOTE, "nvme_prp_pool_init: allocating PRP pool (%d pages, %d bytes)",
            pages, pages * NBPP);
#endif
    soft->prp_pool = kvpalloc(pages,
                              VM_UNCACHED | VM_PHYSCONTIG | VM_DIRECT | VM_NOSLEEP,
                              0);
    if (!soft->prp_pool) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_prp_pool_init: failed to allocate PRP pool");
#endif
        return -1;
    }

    /* Clear the pool memory */
    bzero(soft->prp_pool, pages * NBPP);

    /* Get DMA-translated physical address for the pool */
    soft->prp_pool_phys = pciio_dmatrans_addr(soft->pci_vhdl, 0,
                                              kvtophys(soft->prp_pool),
                                              pages * NBPP,
                                              PCIIO_DMA_CMD | DMATRANS64 | QUEUE_SWAP);
    if (!soft->prp_pool_phys) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_prp_pool_init: DMA translation failed");
#endif
        kvpfree(soft->prp_pool, pages);
        soft->prp_pool = NULL;
        return -1;
    }

    /* Initialize bitmap - all pages available (all bits set to 1) */
    soft->prp_pool_bitmap = 0xFFFFFFFFFFFFFFFFULL;

    /* Initialize lock */
    init_mutex(&soft->prp_pool_lock, MUTEX_DEFAULT, "nvme_prp_pool", 0);

#ifdef NVME_DBG
    cmn_err(CE_NOTE, "nvme_prp_pool_init: PRP pool allocated at virt=%p phys=0x%llx",
            soft->prp_pool, soft->prp_pool_phys);
#endif
    return 0;
}

/*
 * nvme_prp_pool_done: Free the PRP list pool
 *
 * Releases all resources allocated by nvme_prp_pool_init().
 * Should be called during driver shutdown.
 */
void
nvme_prp_pool_done(nvme_soft_t *soft)
{
    if (!soft->prp_pool) {
        return;  /* Pool was never initialized */
    }

#ifdef NVME_DBG
    cmn_err(CE_NOTE, "nvme_prp_pool_done: freeing PRP pool");
#endif
    /* Destroy the mutex */
    mutex_destroy(&soft->prp_pool_lock);

    /* Free the pool memory */
    kvpfree(soft->prp_pool, NVME_PRP_POOL_SIZE);
    soft->prp_pool = NULL;
    soft->prp_pool_phys = 0;
    soft->prp_pool_bitmap = 0;
}

/*
 * nvme_prp_pool_alloc: Allocate a PRP list page from the pool
 *
 * Finds an available page in the PRP pool bitmap and marks it as allocated.
 *
 * Returns:
 *   0-63: Index of allocated page
 *   -1: No pages available
 */
int
nvme_prp_pool_alloc(nvme_soft_t *soft)
{
    int i;
    __uint64_t mask;

    mutex_lock(&soft->prp_pool_lock, PZERO);

    /* Find first empty bit (1 = available, 0 = in use) */
    for (i = 0; i < NVME_PRP_POOL_SIZE; i++) {
        mask = 1ULL << i;
        if (soft->prp_pool_bitmap & mask) {
            /* Found available page - mark as in use */
            soft->prp_pool_bitmap &= ~mask;
            mutex_unlock(&soft->prp_pool_lock);
            return i;
        }
    }

    /* No pages available */
    mutex_unlock(&soft->prp_pool_lock);
    return -1;
}

/*
 * nvme_prp_pool_free: Free a PRP list page back to the pool
 *
 * Marks the specified page as available in the bitmap.
 *
 * Arguments:
 *   soft  - Controller state
 *   index - Page index (0-63)
 */
void
nvme_prp_pool_free(nvme_soft_t *soft, int index)
{
    __uint64_t mask;

    if (index < 0 || index >= NVME_PRP_POOL_SIZE) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_prp_pool_free: invalid index %d", index);
#endif
        return;
    }

    mutex_lock(&soft->prp_pool_lock, PZERO);

    /* Mark page as available (set bit to 1) */
    mask = 1ULL << index;
    soft->prp_pool_bitmap |= mask;

    mutex_unlock(&soft->prp_pool_lock);
}

/*
 * nvme_io_cid_alloc: Allocate multiple CIDs for I/O commands
 *
 * Finds free CID slots in the I/O queue, marks them as allocated,
 * and stores the scsi_request pointer for later retrieval.
 * Stores the reference count in req->sr_ha.
 *
 * Bitmap semantics: 0 = free, 1 = occupied
 *
 * Arguments:
 *   soft      - Controller state
 *   req       - SCSI request structure
 *   commands  - Number of CIDs to allocate
 *   cid_array - Output array to store allocated CIDs (must have space for 'commands' entries)
 *
 * Returns:
 *   0 on success (all CIDs allocated)
 *   -1 on failure (not enough free CIDs available, none allocated)
 */
int
nvme_io_cid_alloc(nvme_soft_t *soft, scsi_request_t *req, unsigned int commands, unsigned int *cid_array)
{
    unsigned int allocated = 0;
    unsigned int word_idx;
    unsigned int bit_idx;
    unsigned int word;
    unsigned int mask;
    unsigned int cid;
    int i;

    if (commands == 0) {
        return -1;
    }

    mutex_lock(&soft->io_requests_lock, PZERO);

    /* Search for free bits in the bitmap (8 words x 32 bits = 256 bits) */
    for (word_idx = 0; word_idx < 8 && allocated < commands; word_idx++) {
        word = soft->io_cid_bitmap[word_idx];

        /* If word is all ones, no free slots in this word */
        if (word == 0xFFFFFFFF)
            continue;

        /* Find zero bits (free CIDs) in this word */
        for (bit_idx = 0; bit_idx < 32 && allocated < commands; bit_idx++) {
            mask = 1u << bit_idx;
            if (!(word & mask)) {
                /* Found a free CID - calculate CID number */
                cid = ((word_idx << 5u) + bit_idx);

                /* Set the bit to mark as occupied */
                soft->io_cid_bitmap[word_idx] |= mask;

                /* Store CID in output array */
                cid_array[allocated] = cid;
                allocated++;

                /* Update the word variable for next iteration */
                word = soft->io_cid_bitmap[word_idx];
            }
        }
    }

    /* Check if we allocated all requested CIDs */
    if (allocated < commands) {
        /* Not enough free CIDs - rollback all allocations */
        for (i = 0; i < allocated; i++) {
            cid = cid_array[i];
            word_idx = cid >> 5u;
            bit_idx = cid & 0x1F;
            mask = 1u << bit_idx;
            soft->io_cid_bitmap[word_idx] &= ~mask;
        }
        mutex_unlock(&soft->io_requests_lock);
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_io_cid_alloc: not enough free CIDs (requested %u, found %u)",
                commands, allocated);
#endif
        return -1;
    }

    mutex_unlock(&soft->io_requests_lock);

    /* Initialize all allocated CID slots (outside lock since we own them) */
    for (i = 0; i < commands; i++) {
        cid = cid_array[i];
        soft->io_requests[cid].req = req;
        for (word_idx = 0; word_idx < NVME_CMD_MAX_PRPS; word_idx++) {
            soft->io_requests[cid].prpidx[word_idx] = -1;
        }
    }

    /* Store reference count in sr_ha (cast to void pointer) */
    req->sr_ha = (void *)(__psint_t)commands;

    return 0;
}

/*
 * nvme_io_cid_done: Free a CID and PRP and retrieve req.
 *
 * Marks the CID as free and clears the scsi_request pointer.
 * Frees the PRP if attached.
 * Decrements the reference count in req->sr_ha. Only returns the req
 * when the reference count reaches zero (all commands completed).
 * Bitmap semantics: 0 = free, 1 = occupied
 *
 * Returns:
 *   scsi_request_t* if this was the last CID (refcount reached 0)
 *   NULL if there are still outstanding CIDs for this request
 */
scsi_request_t *
nvme_io_cid_done(nvme_soft_t *soft, unsigned int cid)
{
    scsi_request_t *req;
    unsigned int word_idx;
    unsigned int bit_idx;
    unsigned int mask;
    unsigned int refcount;
    int i;

    if (cid >= NVME_IO_QUEUE_SIZE) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_io_cid_free: invalid CID %d", cid);
#endif
        return NULL;
    }

    word_idx = cid >> 5u;       /* Divide by 32 */
    bit_idx = cid & 0x1F;       /* Modulo 32 */
    mask = 1u << bit_idx;

    req = soft->io_requests[cid].req;

    /* Clear the scsi_request pointer */
    soft->io_requests[cid].req = NULL;

    /* Free PRP storage */
    for (i = 0; i < NVME_CMD_MAX_PRPS; i++) {
        if (soft->io_requests[cid].prpidx[i] >= 0) {
            nvme_prp_pool_free(soft, soft->io_requests[cid].prpidx[i]);
            soft->io_requests[cid].prpidx[i] = -1;
        }
    }

    mutex_lock(&soft->io_requests_lock, PZERO);
    /* Clear the bit to mark as free */
    soft->io_cid_bitmap[word_idx] &= ~mask;
    mutex_unlock(&soft->io_requests_lock);

    /* Decrement reference count and check if this was the last one */
    if (req != NULL) {
        /* Get current refcount from sr_ha */
        refcount = (__psint_t)req->sr_ha;

        /* Decrement (single-threaded completion, no need for atomics) */
        refcount--;
        req->sr_ha = (void *)(__psint_t)refcount;

#ifdef NVME_DBG_EXTRA
        cmn_err(CE_NOTE, "nvme_io_cid_done: CID %u done, refcount now %u", cid, refcount);
#endif

        /* Only return req if all commands are done */
        if (refcount == 0) {
            /* Clear sr_ha before returning (required before sr_notify) */
            req->sr_ha = NULL;
            return req;
        }
    }

    return NULL;
}

/*
 Store PRP index in the array so we can have more than one PRP page for request
*/
int
nvme_io_cid_store_prp(nvme_soft_t *soft, unsigned int cid, int prpidx)
{
    int i;
    for (i = 0; i < NVME_CMD_MAX_PRPS; i++)
        if (soft->io_requests[cid].prpidx[i] == -1)
        {
            soft->io_requests[cid].prpidx[i] = prpidx;
            return 0;
        }
    return -1;
}

/*
 * nvme_cmd_special_flush: Issue a special flush command (not tied to scsi_request)
 *
 * This is used for ordering guarantees when processing ordered or head-of-queue
 * commands. The flush uses a special CID (NVME_IO_CID_FLUSH) that won't conflict
 * with normal I/O CIDs (0-255).
 *
 * Returns:
 *   0 on success (command submitted)
 *   -1 on failure
 */
int
nvme_cmd_special_flush(nvme_soft_t *soft)
{
    nvme_command_t cmd;
    int rc;

    /* Build NVMe FLUSH command */
    bzero(&cmd, sizeof(cmd));

    /* CDW0: Opcode (7:0), Flags (15:8), CID (31:16) */
    cmd.cdw0 = NVME_CMD_FLUSH | (NVME_IO_CID_FLUSH << 16);

    /* Set namespace ID to 1 (we always use namespace 1) */
    cmd.nsid = 1;

    /* Submit the command to the I/O queue */
    rc = nvme_submit_cmd(soft, &soft->io_queue, &cmd);
    if (rc != 0) {
#ifdef NVME_DBG
        cmn_err(CE_WARN, "nvme_cmd_special_flush: failed to submit special flush command");
#endif
        return -1;
    }

#ifdef NVME_DBG_EXTRA
    cmn_err(CE_NOTE, "nvme_cmd_special_flush: submitted special flush with CID 0x%x",
            NVME_IO_CID_FLUSH);
#endif
    return 0;
}

#ifdef NVME_TEST
void
nvme_cmd_admin_test(nvme_soft_t *soft, unsigned int i)
{
    nvme_command_t cmd;

    cmn_err(CE_NOTE, "nvme_admin_identify_controller: sending command");

    /* Clear utility buffer */
    bzero(soft->utility_buffer, NBPP);

    /* Build Identify Controller command */
    bzero(&cmd, sizeof(cmd));

    /* CDW0: Opcode (7:0), Flags (15:8), CID (31:16) */
    cmd.cdw0 = NVME_ADMIN_IDENTIFY | (i << 16);

    /* NSID: not used for controller identify */
    cmd.nsid = 0;

    /* PRP1: physical address of utility buffer (controller data destination) */
    cmd.prp1_lo = PHYS64_LO(soft->utility_buffer_phys);
    cmd.prp1_hi = PHYS64_HI(soft->utility_buffer_phys);

    /* PRP2: not needed (data is only 4KB) */
    cmd.prp2_lo = 0;
    cmd.prp2_hi = 0;

    /* CDW10: CNS = 0x01 for Identify Controller */
    cmd.cdw10 = NVME_CNS_CONTROLLER;

    /* Submit command */
    if (nvme_submit_cmd(soft, &soft->admin_queue, &cmd) != 0) {
        cmn_err(CE_WARN, "nvme_cmd_admin_test: failed to submit command (queue full?)");
        return;  /* Failure */
    }
}

void
nvme_cmd_io_test(nvme_soft_t *soft, unsigned int i)
{
    nvme_command_t cmd;

    cmn_err(CE_NOTE, "nvme_admin_identify_controller: sending command");

    /* Clear utility buffer */
    bzero(soft->utility_buffer, NBPP);

    /* Build Identify Controller command */
    bzero(&cmd, sizeof(cmd));

    cmd.nsid = 1;

#if 1
    /* CDW0: Opcode (7:0), Flags (15:8), CID (31:16) */
    cmd.cdw0 = NVME_CMD_READ | (i << 16);
    cmd.cdw10 = 0; // lba low
    cmd.cdw11 = 0; // lba hi
    cmd.cdw12 = 0; // blocks
    cmd.prp1_lo = PHYS64_LO(soft->utility_buffer_phys);
    cmd.prp1_hi = PHYS64_HI(soft->utility_buffer_phys);
#endif

#if 0
    /* CDW0: Opcode (7:0), Flags (15:8), CID (31:16) */
    cmd.cdw0 = NVME_CMD_VERIFY | (i << 16);
    cmd.cdw10 = 0; // lba low
    cmd.cdw11 = 0; // lba hi
    cmd.cdw12 = 0; // blocks - 1, LR, FUA, PRINFO = 0
#endif

#if 0
    /* CDW0: Opcode (7:0), Flags (15:8), CID (31:16) */
    cmd.cdw0 = NVME_CMD_FLUSH | (i << 16);
#endif


    /* Submit command */
    if (nvme_submit_cmd(soft, &soft->io_queue, &cmd) != 0) {
        cmn_err(CE_WARN, "nvme_cmd_io_test: failed to submit command (queue full?)");
        return;  /* Failure */
    }
}

#endif
