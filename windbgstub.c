#include "qemu/osdep.h"
#include "cpu.h"
#include "sysemu/char.h"
#include "sysemu/sysemu.h"
#include "qemu/error-report.h"
#include "exec/windbgstub.h"
#include "exec/windbgstub-utils.h"

#define WINDBG "windbg"

//windbg.exe -b -k com:pipe,baud=115200,port=\\.\pipe\windbg,resets=0
//qemu.exe -windbg pipe:windbg

static uint32_t cntrl_packet_id = RESET_PACKET_ID;
static uint32_t data_packet_id = INITIAL_PACKET_ID | SYNC_PACKET_ID;
static uint8_t lock = 0;
static bool bp = 0;

typedef enum ParsingState {
    STATE_LEADER,
    STATE_PACKET_TYPE,
    STATE_PACKET_BYTE_COUNT,
    STATE_PACKET_ID,
    STATE_PACKET_CHECKSUM,
    STATE_PACKET_DATA,
    STATE_TRAILING_BYTE,
} ParsingState;

typedef struct Context {
    // index in the current buffer,
    // which depends on the current state
    int index;
    ParsingState state;
    KD_PACKET packet;
    uint8_t data[PACKET_MAX_SIZE];
} Context;

static bool is_debug = false;
static uint32_t cntrl_packet_id = RESET_PACKET_ID;
static uint32_t data_packet_id = INITIAL_PACKET_ID;
static uint8_t lock = 0;
static bool bp = false;

static Context input_context = { .state = STATE_LEADER };

static CharDriverState *windbg_chr = NULL;
static CharDriverState *serial_chr = NULL;

static FILE *dump_file;

static bool is_debug = false;

//TODO: Remove it
static uint32_t cntrl_packet_id = RESET_PACKET_ID;
static uint32_t data_packet_id = INITIAL_PACKET_ID | SYNC_PACKET_ID;
static uint8_t lock = 0;
//////////////////////////////////////////////////

static PCPU_CTRL_ADDRS pc_addrs;

static void windbg_dump(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (dump_file) {
        vfprintf(dump_file, fmt, ap);
        fflush(dump_file);
    }
    va_end(ap);
}

static void windbg_send_data_packet(uint8_t *data, uint16_t byte_count,
                                    uint16_t type)
{
    uint8_t trailing_byte = PACKET_TRAILING_BYTE;

    KD_PACKET packet = {
        .PacketLeader = PACKET_LEADER,
        .PacketType = type,
        .ByteCount = byte_count,
        .PacketId = data_packet_id,
        .Checksum = data_checksum_compute(data, byte_count)
    };

    qemu_chr_fe_write(windbg_chr, (uint8_t *)&packet,
        sizeof(packet));
    qemu_chr_fe_write(windbg_chr, data, byte_count);
    qemu_chr_fe_write(windbg_chr, &trailing_byte,
        sizeof(trailing_byte));

    data_packet_id ^= 1;

    DUMP_STRUCT(packet);
    DUMP_ARRAY(data, byte_count);
    DUMP_VAR(trailing_byte);
}

static void windbg_send_control_packet(uint16_t type)
{
    KD_PACKET packet = {
        .PacketLeader = CONTROL_PACKET_LEADER,
        .PacketType = type,
        .ByteCount = 0,
        .PacketId = cntrl_packet_id,
        .Checksum = 0
    };

    qemu_chr_fe_write(windbg_chr, (uint8_t *)&packet, sizeof(packet));

    cntrl_packet_id ^= 1;

    DUMP_STRUCT(packet);
}

static void windbg_process_manipulate_packet(Context *ctx)
{
    uint8_t packet[PACKET_MAX_SIZE];
    size_t packet_size = 0,
           extra_data_size = 0,
           m64_size = sizeof(DBGKD_MANIPULATE_STATE64);
    uint32_t count, addr;
    static uint64_t bp_addr = 0;
    bool send_only_m64 = false;
    DBGKD_MANIPULATE_STATE64 m64;
    CPUState *cpu = qemu_get_cpu(0);

    memset(packet, 0, PACKET_MAX_SIZE);
    memcpy(&m64, ctx->data, m64_size);

    extra_data_size = ctx->packet.ByteCount - m64_size;

    m64.ReturnStatus = 0x0;

    switch(m64.ApiNumber) {

    case DbgKdReadVirtualMemoryApi:
        count = m64.u.ReadMemory.TransferCount;
        addr = m64.u.ReadMemory.TargetBaseAddress;

        m64.u.ReadMemory.ActualBytesRead = count;
        cpu_memory_rw_debug(cpu, addr, M64_OFFSET(packet), count, 0);
        packet_size = m64_size + count;
        
        break;
    case DbgKdWriteVirtualMemoryApi:
        count = ROUND(extra_data_size, m64.u.WriteMemory.TransferCount);
        addr = m64.u.WriteMemory.TargetBaseAddress;

        m64.u.WriteMemory.ActualBytesWritten = count;
        cpu_memory_rw_debug(cpu, addr, M64_OFFSET(ctx->data), count, 1);

        send_only_m64 = true;
        
        break;
    case DbgKdGetContextApi:
    {
        //TODO: For all processors
        PCPU_CONTEXT cpuctx = get_Context(0);
        
        packet_size = sizeof(CPU_CONTEXT);
        memcpy(M64_OFFSET(packet), cpuctx, packet_size);
        packet_size += m64_size;

        break;
    }
    case DbgKdSetContextApi:
        set_Context(M64_OFFSET(ctx->data), ROUND(extra_data_size,
            sizeof(CPU_CONTEXT)), 0);

        send_only_m64 = true;
        
        break;
    case DbgKdWriteBreakPointApi:
        bp_addr = m64.u.WriteBreakPoint.BreakPointAddress & 0xffffffff;
        
        m64.u.WriteBreakPoint.BreakPointHandle = 0x1;
        cpu_breakpoint_insert(cpu, bp_addr, BP_GDB, NULL);
        
        send_only_m64 = true;   
    
        break;
    case DbgKdRestoreBreakPointApi:
        m64.ReturnStatus = 0xc0000001;
        if (bp_addr) {
            cpu_breakpoint_remove(cpu, bp_addr, BP_GDB);
            bp_addr = 0;
        }
        
        send_only_m64 = true;
        
        break;
    case DbgKdContinueApi:
        send_only_m64 = true;
        
        break;
    case DbgKdReadControlSpaceApi:
    {
        //TODO: For all processors
        PCPU_KSPECIAL_REGISTERS ksreg = get_KSpecialRegisters(0);
         
        count = m64.u.ReadMemory.TransferCount;
        addr = m64.u.ReadMemory.TargetBaseAddress - sizeof(CPU_CONTEXT);

        m64.u.ReadMemory.ActualBytesRead = count;
        memcpy(M64_OFFSET(packet), ((uint8_t *) ksreg) + addr, count);
        packet_size = m64_size + count;
        
        break;
    }
    case DbgKdWriteControlSpaceApi:
        count = ROUND(extra_data_size, m64.u.WriteMemory.TransferCount);
        addr = m64.u.WriteMemory.TargetBaseAddress - sizeof(CPU_CONTEXT);

        m64.u.WriteMemory.ActualBytesWritten = count;
        set_KSpecialRegisters(M64_OFFSET(ctx->data), count, addr, 0);

        send_only_m64 = true;
        
        break;
    case DbgKdReadIoSpaceApi:

        break;
    case DbgKdWriteIoSpaceApi:

        break;
    case DbgKdRebootApi:

        break;
    case DbgKdContinueApi2:
    {
        uint32_t tf = m64.u.Continue2.ControlSet.TraceFlag;

        if (!tf) {
            bp = false;
        }
        vm_start();

        if (tf) {
            windbg_send_data_packet((uint8_t *)get_ExceptionStateChange(0),
                sizeof(EXCEPTION_STATE_CHANGE),
                PACKET_TYPE_KD_STATE_CHANGE64);
        }
        
        return;
    }
    case DbgKdReadPhysicalMemoryApi:

        break;
    case DbgKdWritePhysicalMemoryApi:

        break;
    case DbgKdQuerySpecialCallsApi:

        break;
    case DbgKdSetSpecialCallApi:

        break;
    case DbgKdClearSpecialCallsApi:

        break;
    case DbgKdSetInternalBreakPointApi:

        break;
    case DbgKdGetInternalBreakPointApi:

        break;
    case DbgKdReadIoSpaceExtendedApi:

        break;
    case DbgKdWriteIoSpaceExtendedApi:

        break;
    case DbgKdGetVersionApi:
        cpu_memory_rw_debug(cpu, cc_addrs->Version, PTR(m64) + 0x10,
            m64_size - 0x10, 0);

        send_only_m64 = true;
        
        break;
    case DbgKdWriteBreakPointExApi:

        break;
    case DbgKdRestoreBreakPointExApi:

        break;
    case DbgKdCauseBugCheckApi:

        break;
    case DbgKdSwitchProcessor:

        break;
    case DbgKdPageInApi:

        break;
    case DbgKdReadMachineSpecificRegister:

        break;
    case DbgKdWriteMachineSpecificRegister:

        break;
    case OldVlm1:

        break;
    case OldVlm2:

        break;
    case DbgKdSearchMemoryApi:

        break;
    case DbgKdGetBusDataApi:

        break;
    case DbgKdSetBusDataApi:

        break;
    case DbgKdCheckLowMemoryApi:

        break;
    case DbgKdClearAllInternalBreakpointsApi:

        return;
    case DbgKdFillMemoryApi:

        break;
    case DbgKdQueryMemoryApi:

        break;
    case DbgKdSwitchPartition:

        break;
    default:

        break;
    }

    if (send_only_m64) {
        windbg_send_data_packet(PTR(m64), m64_size, ctx->packet.PacketType);
    }
    else {
        memcpy(packet, &m64, m64_size);
        windbg_send_data_packet(packet, packet_size, ctx->packet.PacketType);
    }
}

static void windbg_process_data_packet(Context *ctx)
{
    switch (ctx->packet.PacketType) {
    case PACKET_TYPE_KD_STATE_MANIPULATE:
        windbg_send_control_packet(PACKET_TYPE_KD_ACKNOWLEDGE);
        windbg_process_manipulate_packet(ctx);

        break;
    default:
        cntrl_packet_id = 0;
        windbg_send_control_packet(PACKET_TYPE_KD_RESEND);

        break;
    }
}

static void windbg_process_control_packet(Context *ctx)
{
    switch (ctx->packet.PacketType) {
    case PACKET_TYPE_UNUSED:

        break;
    case PACKET_TYPE_KD_STATE_CHANGE32:

        break;
    case PACKET_TYPE_KD_STATE_MANIPULATE:

        break;
    case PACKET_TYPE_KD_DEBUG_IO:

        break;
    case PACKET_TYPE_KD_ACKNOWLEDGE:

        break;
    case PACKET_TYPE_KD_RESEND:

        break;
    case PACKET_TYPE_KD_RESET:
    {
        //TODO: For all processors
        uint8_t *data = get_LoadSymbolsStateChange(0);
        
        windbg_send_data_packet(data, get_lssc_size(), 
            PACKET_TYPE_KD_STATE_CHANGE64);
        windbg_send_control_packet(ctx->packet.PacketType);
        cntrl_packet_id = INITIAL_PACKET_ID;

        break;
    }
    case PACKET_TYPE_KD_STATE_CHANGE64:

        break;
    case PACKET_TYPE_KD_POLL_BREAKIN:

        break;
    case PACKET_TYPE_KD_TRACE_IO:

        break;
    case PACKET_TYPE_KD_CONTROL_REQUEST:

        break;
    case PACKET_TYPE_KD_FILE_IO:

        break;
    case PACKET_TYPE_MAX:

        break;
    default:
        cntrl_packet_id = 0;
        windbg_send_control_packet(PACKET_TYPE_KD_RESEND);

        break;
    }
}

static int windbg_chr_can_receive(void *opaque)
{
  /* We can handle an arbitrarily large amount of data.
   Pick the maximum packet size, which is as good as anything.  */
  return PACKET_MAX_SIZE;
}

void windbg_set_bp(int index)
{
    windbg_send_data_packet((uint8_t *)get_ExceptionStateChange(0),
                            sizeof(EXCEPTION_STATE_CHANGE),
                            PACKET_TYPE_KD_STATE_CHANGE64);
    vm_stop(RUN_STATE_PAUSED);
    bp = true;
}

static void windbg_read_byte(Context *ctx, uint8_t byte)
{
    switch (ctx->state) {
    case STATE_LEADER:
        if (byte == PACKET_LEADER_BYTE || byte == CONTROL_PACKET_LEADER_BYTE) {
            if (ctx->index > 0 && byte != BYTE(ctx->packet.PacketLeader, 0)) {
                ctx->index = 0;
            }
            BYTE(ctx->packet.PacketLeader, ctx->index) = byte;
            ++ctx->index;
            if (ctx->index == sizeof(ctx->packet.PacketLeader)) {
                ctx->state = STATE_PACKET_TYPE;
                ctx->index = 0;
            }
        } else if (byte == BREAKIN_PACKET_BYTE) {
            //TODO: For all processors
            windbg_set_bp(0);
            ctx->index = 0;
        } else {
            // skip the byte, restart waiting for the leader
            ctx->index = 0;
        }
        break;
    case STATE_PACKET_TYPE:
        BYTE(ctx->packet.PacketType, ctx->index) = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.PacketType)) {
            if (ctx->packet.PacketType >= PACKET_TYPE_MAX) {
                ctx->state = STATE_LEADER;
            } else {
                if (ctx->packet.PacketLeader == CONTROL_PACKET_LEADER
                    && ctx->packet.PacketType == PACKET_TYPE_KD_RESEND) {
                    ctx->state = STATE_LEADER;
                } else {
                    ctx->state = STATE_PACKET_BYTE_COUNT;
                }
            }
            ctx->index = 0;
        }
        break;
    case STATE_PACKET_BYTE_COUNT:
        BYTE(ctx->packet.ByteCount, ctx->index) = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.ByteCount)) {
            ctx->state = STATE_PACKET_ID;
            ctx->index = 0;
        }
        break;
    case STATE_PACKET_ID:
        BYTE(ctx->packet.PacketId, ctx->index) = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.PacketId)) {
            ctx->state = STATE_PACKET_CHECKSUM;
            ctx->index = 0;
        }
        break;
    case STATE_PACKET_CHECKSUM:
        BYTE(ctx->packet.Checksum, ctx->index) = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.Checksum)) {
            if (ctx->packet.PacketLeader == CONTROL_PACKET_LEADER) {
                windbg_process_control_packet(ctx);
                ctx->state = STATE_LEADER;
            } else {
                if (ctx->packet.ByteCount > PACKET_MAX_SIZE) {
                    ctx->state = STATE_LEADER;
                    cntrl_packet_id = 0;
                    windbg_send_control_packet(PACKET_TYPE_KD_RESEND);
                } else {
                    ctx->state = STATE_PACKET_DATA;
                }
            }
            ctx->index = 0;
        }
        break;
    case STATE_PACKET_DATA:
        ctx->data[ctx->index] = byte;
        ++ctx->index;
        if (ctx->index == ctx->packet.ByteCount) {
            ctx->state = STATE_TRAILING_BYTE;
            ctx->index = 0;
        }
        break;
    case STATE_TRAILING_BYTE:
        if (byte == PACKET_TRAILING_BYTE) {
            windbg_process_data_packet(ctx);
        } else {
            cntrl_packet_id = 0;
            windbg_send_control_packet(PACKET_TYPE_KD_RESEND);
        }
        ctx->state = STATE_LEADER;
        break;
    }
}

static void windbg_receive_from_windbg(void *opaque, const uint8_t *buf, int size)
{
    DUMP_ARRAY(buf, size);
    if (!is_debug) {
        if (lock) {
            int i;
            for (i = 0; i < size; i++) {
                windbg_read_byte(&input_context, buf[i]);
            }
        }
    }
    else {
        uint8_t *tmp = g_malloc(size);
        memcpy(tmp, buf, size);
        qemu_chr_be_write(serial_chr, tmp, size);
        g_free(tmp);
    }
}

static int windbg_receive_from_kernel(struct CharDriverState *chr, const uint8_t *buf, int size)
{
    if (is_debug) {
        qemu_chr_fe_write(windbg_chr, buf, size);
    }
    return size;
}

bool windbg_check_bp(void)
{
    return bp;
}

void windbg_start_sync(void)
{
    get_init();
    cc_addrs = get_KPCRAddress(0);
    
    lock = 1;
}

static void windbg_close(void)
{
    if (dump_file) {
        fclose(dump_file);
    }
    dump_file = NULL;
}

static void windbg_exit(void)
{
    get_free();
    windbg_close();
}

static void windbg_serial_parse(QemuOpts *opts, ChardevBackend *backend,
                                  Error **errp)
{
    ChardevHostdev *serial;
    serial = backend->u.serial.data = g_new0(ChardevHostdev, 1);
    qemu_chr_parse_common(opts, qapi_ChardevHostdev_base(serial));
    serial->device = g_strdup(WINDBG);
}

static CharDriverState *windbg_serial_open(const char *id, ChardevBackend *backend, ChardevReturn *ret, Error **errp)
{
    if (serial_chr) {
        error_report("WinDbg: Multiple instances are not supported yet");
        return NULL;
    }

    ChardevHostdev *opts = backend->u.windbg.data;
    ChardevCommon *common = qapi_ChardevHostdev_base(opts);

    serial_chr = qemu_chr_alloc(common, errp);
    if (!serial_chr) {
        error_report("WinDbg: problem with allocation serial chr");
        return NULL;
    }

    serial_chr->chr_write = windbg_receive_from_kernel;
    return serial_chr;
}

int windbgserver_start(const char *device)
{
    if (windbg_chr) {
        error_report("WinDbg: Multiple instances are not supported yet");
        exit(1);
    }

    const char *p;
    is_debug = strstart(device, "debug,", &p);
    if (is_debug) {
        device = p;
    }

    register_char_driver(WINDBG, CHARDEV_BACKEND_KIND_WINDBG,
                            windbg_serial_parse, windbg_serial_open);

    // open external pipe for listening to windbg
    windbg_chr = qemu_chr_new(WINDBG, device, NULL);
    if (!windbg_chr) {
        return -1;
    }

    qemu_chr_fe_claim_no_fail(windbg_chr);
    qemu_chr_add_handlers(windbg_chr, windbg_chr_can_receive,
                          windbg_receive_from_windbg, NULL, NULL);

    // open dump file
    dump_file = fopen(WINDBG ".dump", "wb");

    atexit(windbg_exit);

    return 0;
}