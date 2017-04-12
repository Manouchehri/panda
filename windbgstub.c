#include "qemu/osdep.h"
#include "sysemu/char.h"
#include "sysemu/sysemu.h"
#include "exec/windbgstub.h"
#include "exec/windbgstub-utils.h"
#include "exec/address-spaces.h"

#define ENABLE_PARSER        WINDBG_DEBUG_ON && (ENABLE_WINDBG_PARSER || ENABLE_KERNEL_PARSER)
#define ENABLE_WINDBG_PARSER true
#define ENABLE_KERNEL_PARSER true
#define ENABLE_FULL_HANDLER  ENABLE_PARSER && true
#define ENABLE_API_HANDLER   ENABLE_PARSER && true

#define WINDBG_DIR CONFIG_QEMU_DATADIR "/" WINDBG "_"

typedef enum ParsingState {
    STATE_LEADER,
    STATE_PACKET_TYPE,
    STATE_PACKET_BYTE_COUNT,
    STATE_PACKET_ID,
    STATE_PACKET_CHECKSUM,
    STATE_PACKET_DATA,
    STATE_TRAILING_BYTE,
} ParsingState;

typedef enum ParsingResult {
    RESULT_NONE,
    RESULT_BREAKIN_BYTE,
    RESULT_UNKNOWN_PACKET,
    RESULT_CONTROL_PACKET,
    RESULT_DATA_PACKET,
    RESULT_ERROR,
} ParsingResult;

typedef struct ParsingContext {
    // index in the current buffer,
    // which depends on the current state
    int index;
    ParsingState state;
    ParsingResult result;
    KD_PACKET packet;
    PacketData data;
    const char *name;
} ParsingContext;

typedef struct WindbgState {
    CharDriverState *chr;

    uint32_t ctrl_packet_id;
    uint32_t data_packet_id;
    bool is_loaded;

  #if (WINDBG_DEBUG_ON)
    FILE *dump_file;
  #endif
} WindbgState;

static WindbgState *windbg_state = NULL;
#if (ENABLE_PARSER)
    FILE *parsed_packets;
    FILE *parsed_api;
#endif

void windbg_dump(const char *fmt, ...)
{
  #if (WINDBG_DEBUG_ON)
    va_list ap;

    va_start(ap, fmt);
    if (windbg_state->dump_file) {
        vfprintf(windbg_state->dump_file, fmt, ap);
        fflush(windbg_state->dump_file);
    }
    va_end(ap);
  #endif
}

static void windbg_send_data_packet(uint8_t *data, uint16_t byte_count, uint16_t type)
{
    uint8_t trailing_byte = PACKET_TRAILING_BYTE;

    KD_PACKET packet = {
        .PacketLeader = PACKET_LEADER,
        .PacketType = type,
        .ByteCount = byte_count,
        .PacketId = windbg_state->data_packet_id,
        .Checksum = compute_checksum(data, byte_count)
    };

    qemu_chr_fe_write(windbg_state->chr, PTR(packet), sizeof(packet));
    qemu_chr_fe_write(windbg_state->chr, data, byte_count);
    qemu_chr_fe_write(windbg_state->chr, &trailing_byte, sizeof(trailing_byte));

    windbg_state->data_packet_id ^= 1;
}

static void windbg_send_control_packet(uint16_t type)
{
    KD_PACKET packet = {
        .PacketLeader = CONTROL_PACKET_LEADER,
        .PacketType = type,
        .ByteCount = 0,
        .PacketId = windbg_state->ctrl_packet_id,
        .Checksum = 0
    };

    qemu_chr_fe_write(windbg_state->chr, PTR(packet), sizeof(packet));

    windbg_state->ctrl_packet_id ^= 1;
}

static int windbg_chr_can_receive(void *opaque)
{
    return PACKET_MAX_SIZE;
}

static void windbg_bp_handler(CPUState *cpu)
{
    SizedBuf buf = kd_gen_exception_sc(cpu);
    windbg_send_data_packet(buf.data, buf.size, PACKET_TYPE_KD_STATE_CHANGE64);
    SBUF_FREE(buf);
}

static void windbg_vm_stop(void)
{
    CPUState *cpu = qemu_get_cpu(0);
    vm_stop(RUN_STATE_PAUSED);
    cpu_single_step(cpu, 0);
    windbg_bp_handler(cpu);
}

static void windbg_process_manipulate_packet(ParsingContext *ctx)
{
    ctx->data.extra_size = ctx->packet.ByteCount - M64_SIZE;
    ctx->data.m64.ReturnStatus = STATUS_SUCCESS;

    CPUState *cpu = qemu_get_cpu(ctx->data.m64.Processor < get_cpu_amount() ?
                                 ctx->data.m64.Processor : 0);

    switch(ctx->data.m64.ApiNumber) {

    case DbgKdReadVirtualMemoryApi:
        kd_api_read_virtual_memory(cpu, &ctx->data);
        break;

    case DbgKdWriteVirtualMemoryApi:
        kd_api_write_virtual_memory(cpu, &ctx->data);
        break;

    case DbgKdGetContextApi:
        kd_api_get_context(cpu, &ctx->data);
        break;

    case DbgKdSetContextApi:
        kd_api_set_context(cpu, &ctx->data);
        break;

    case DbgKdWriteBreakPointApi:
        kd_api_write_breakpoint(cpu, &ctx->data);
        break;

    case DbgKdRestoreBreakPointApi:
        kd_api_restore_breakpoint(cpu, &ctx->data);
        break;

    case DbgKdReadControlSpaceApi:
        kd_api_read_control_space(cpu, &ctx->data);
        break;

    case DbgKdWriteControlSpaceApi:
        kd_api_write_control_space(cpu, &ctx->data);
        break;

    case DbgKdReadIoSpaceApi:
        kd_api_read_io_space(cpu, &ctx->data);
        break;

    case DbgKdWriteIoSpaceApi:
        kd_api_write_io_space(cpu, &ctx->data);
        break;

    case DbgKdContinueApi:
    case DbgKdContinueApi2:
        kd_api_continue(cpu, &ctx->data);
        return;

    case DbgKdReadPhysicalMemoryApi:
        kd_api_read_physical_memory(cpu, &ctx->data);
        break;

    case DbgKdWritePhysicalMemoryApi:
        kd_api_write_physical_memory(cpu, &ctx->data);
        break;

    case DbgKdGetVersionApi:
        kd_api_get_version(cpu, &ctx->data);
        break;

    case DbgKdReadMachineSpecificRegister:
        kd_api_read_msr(cpu, &ctx->data);
        break;

    case DbgKdWriteMachineSpecificRegister:
        kd_api_write_msr(cpu, &ctx->data);
        break;

    case DbgKdSearchMemoryApi:
        kd_api_search_memory(cpu, &ctx->data);
        break;

    case DbgKdClearAllInternalBreakpointsApi:
        return;

    case DbgKdFillMemoryApi:
        kd_api_fill_memory(cpu, &ctx->data);
        break;

    case DbgKdQueryMemoryApi:
        kd_api_query_memory(cpu, &ctx->data);
        break;

    default:
        kd_api_unsupported(cpu, &ctx->data);
        break;
    }

    windbg_send_data_packet(ctx->data.buf, ctx->data.extra_size + M64_SIZE,
                            ctx->packet.PacketType);
}

static void windbg_process_data_packet(ParsingContext *ctx)
{
    switch (ctx->packet.PacketType) {
    case PACKET_TYPE_KD_STATE_MANIPULATE:
        windbg_send_control_packet(PACKET_TYPE_KD_ACKNOWLEDGE);
        windbg_process_manipulate_packet(ctx);
        break;

    default:
        WINDBG_ERROR("Catched unsupported data packet 0x%x",
                     ctx->packet.PacketType);

        windbg_state->ctrl_packet_id = 0;
        windbg_send_control_packet(PACKET_TYPE_KD_RESEND);
        break;
    }
}

static void windbg_process_control_packet(ParsingContext *ctx)
{
    switch (ctx->packet.PacketType) {
    case PACKET_TYPE_KD_ACKNOWLEDGE:
        break;

    case PACKET_TYPE_KD_RESET:
    {
        SizedBuf buf = kd_gen_load_symbols_sc(qemu_get_cpu(0));

        windbg_send_data_packet(buf.data, buf.size,
                                PACKET_TYPE_KD_STATE_CHANGE64);
        windbg_send_control_packet(ctx->packet.PacketType);
        windbg_state->ctrl_packet_id = INITIAL_PACKET_ID;
        SBUF_FREE(buf);
        break;
    }
    default:
        WINDBG_ERROR("Catched unsupported control packet 0x%x",
                     ctx->packet.PacketType);

        windbg_state->ctrl_packet_id = 0;
        windbg_send_control_packet(PACKET_TYPE_KD_RESEND);
        break;
    }
}

static void windbg_ctx_handler(ParsingContext *ctx)
{
    switch (ctx->result) {
    case RESULT_NONE:
        break;

    case RESULT_BREAKIN_BYTE:
        windbg_vm_stop();
        break;

    case RESULT_CONTROL_PACKET:
        windbg_process_control_packet(ctx);
        break;

    case RESULT_DATA_PACKET:
        windbg_process_data_packet(ctx);
        break;

    case RESULT_UNKNOWN_PACKET:
    case RESULT_ERROR:
        windbg_state->ctrl_packet_id = 0;
        windbg_send_control_packet(PACKET_TYPE_KD_RESEND);
        break;

    default:
        break;
    }
}

static void windbg_read_byte(ParsingContext *ctx, uint8_t byte)
{
    switch (ctx->state) {
    case STATE_LEADER:
        ctx->result = RESULT_NONE;
        if (byte == PACKET_LEADER_BYTE || byte == CONTROL_PACKET_LEADER_BYTE) {
            if (ctx->index > 0 && byte != PTR(ctx->packet.PacketLeader)[0]) {
                ctx->index = 0;
            }
            PTR(ctx->packet.PacketLeader)[ctx->index] = byte;
            ++ctx->index;
            if (ctx->index == sizeof(ctx->packet.PacketLeader)) {
                ctx->state = STATE_PACKET_TYPE;
                ctx->index = 0;
            }
        }
        else if (byte == BREAKIN_PACKET_BYTE) {
            ctx->result = RESULT_BREAKIN_BYTE;
            ctx->index = 0;
        }
        else {
            ctx->index = 0;
        }
        break;

    case STATE_PACKET_TYPE:
        PTR(ctx->packet.PacketType)[ctx->index] = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.PacketType)) {
            if (ctx->packet.PacketType >= PACKET_TYPE_MAX) {
                ctx->state = STATE_LEADER;
                ctx->result = RESULT_UNKNOWN_PACKET;
            }
            else {
                ctx->state = STATE_PACKET_BYTE_COUNT;
            }
            ctx->index = 0;
        }
        break;

    case STATE_PACKET_BYTE_COUNT:
        PTR(ctx->packet.ByteCount)[ctx->index] = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.ByteCount)) {
            ctx->state = STATE_PACKET_ID;
            ctx->index = 0;
        }
        break;

    case STATE_PACKET_ID:
        PTR(ctx->packet.PacketId)[ctx->index] = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.PacketId)) {
            ctx->state = STATE_PACKET_CHECKSUM;
            ctx->index = 0;
        }
        break;

    case STATE_PACKET_CHECKSUM:
        PTR(ctx->packet.Checksum)[ctx->index] = byte;
        ++ctx->index;
        if (ctx->index == sizeof(ctx->packet.Checksum)) {
            if (ctx->packet.PacketLeader == CONTROL_PACKET_LEADER) {
                ctx->state = STATE_LEADER;
                ctx->result = RESULT_CONTROL_PACKET;
            }
            else {
                if (ctx->packet.ByteCount > PACKET_MAX_SIZE) {
                    ctx->state = STATE_LEADER;
                    ctx->result = RESULT_ERROR;
                }
                else {
                    ctx->state = STATE_PACKET_DATA;
                }
            }
            ctx->index = 0;
        }
        break;

    case STATE_PACKET_DATA:
        ctx->data.buf[ctx->index] = byte;
        ++ctx->index;
        if (ctx->index == ctx->packet.ByteCount) {
            ctx->state = STATE_TRAILING_BYTE;
            ctx->index = 0;
        }
        break;

    case STATE_TRAILING_BYTE:
        if (byte == PACKET_TRAILING_BYTE) {
            ctx->result = RESULT_DATA_PACKET;
        }
        else {
            ctx->result = RESULT_ERROR;
        }
        ctx->state = STATE_LEADER;
        break;
    }
}

static void windbg_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    static ParsingContext ctx = {
        .state = STATE_LEADER,
        .result = RESULT_NONE,
        .name = ""
    };

    if (windbg_state->is_loaded) {
        int i;
        for (i = 0; i < size; i++) {
            windbg_read_byte(&ctx, buf[i]);
            windbg_ctx_handler(&ctx);
        }
    }
}

#if (ENABLE_PARSER)

static void windbg_debug_open_files(void)
{
    if (!parsed_packets) {
        parsed_packets = fopen(WINDBG_DIR "parsed_packets.txt", "w");
    }
    if (!parsed_api) {
        parsed_api = fopen(WINDBG_DIR "parsed_api.txt", "w");
    }
}

static void windbg_debug_ctx_handler(ParsingContext *ctx, FILE *out)
{
 #if (ENABLE_FULL_HANDLER)
    if (ctx->result == RESULT_NONE) {
        return;
    }

    fprintf(out, "FROM: %s\n", ctx->name);
    switch (ctx->result) {
    case RESULT_BREAKIN_BYTE:
        fprintf(out, "CATCH BREAKING BYTE\n");
        break;

    case RESULT_UNKNOWN_PACKET:
        fprintf(out, "ERROR: CATCH UNKNOWN PACKET TYPE: 0x%x\n", ctx->packet.PacketType);
        break;

    case RESULT_CONTROL_PACKET:
        fprintf(out, "CATCH CONTROL PACKET: %s\n", kd_get_packet_type_name(ctx->packet.PacketType));
        break;

    case RESULT_DATA_PACKET:
        fprintf(out, "CATCH DATA PACKET: %s\n", kd_get_packet_type_name(ctx->packet.PacketType));
        fprintf(out, "Byte Count: %d\n", ctx->packet.ByteCount);

        if (ctx->packet.PacketType == PACKET_TYPE_KD_STATE_MANIPULATE) {
            fprintf(out, "Api: %s\n", kd_get_api_name(ctx->data.m64.ApiNumber));
        }

        int i;
        for (i = 0; i < ctx->packet.ByteCount; ++i) {
            if (!(i % 16) && i) {
                fprintf(out, "\n");
            }
            fprintf(out, "%02x ", ctx->data.buf[i]);
        }
        fprintf(out, "%saa\n", !(i % 16) ? "\n" : "");
        break;

    case RESULT_ERROR:
        fprintf(out, "ERROR: CATCH ERROR\n");
        break;

    default:
        break;
    }

    fprintf(out, "\n");
    fflush(out);

    if (ctx->data.m64.ApiNumber == DbgKdSetContextApi) {
        vm_stop(RUN_STATE_PAUSED);
    }
 #endif
}

static void windbg_debug_ctx_handler_api(ParsingContext *ctx, FILE *out)
{
 #if (ENABLE_API_HANDLER)
    switch (ctx->result) {
    case RESULT_BREAKIN_BYTE:
        fprintf(out, "BREAKING BYTE\n");
        break;

    case RESULT_DATA_PACKET:
        if (ctx->packet.PacketType == PACKET_TYPE_KD_STATE_MANIPULATE) {
            fprintf(out, "%s: %s\n", ctx->name, kd_get_api_name(ctx->data.m64.ApiNumber));
        }
        break;

    default:
        break;
    }
    fflush(out);
 #endif
}

static void windbg_debug_parser(ParsingContext *ctx, const uint8_t *buf, int len)
{
    int i;
    for (i = 0; i < len; ++i) {
        windbg_read_byte(ctx, buf[i]);
        if (ctx->result != RESULT_NONE) {
            windbg_debug_open_files();
            windbg_debug_ctx_handler(ctx, parsed_packets);
            windbg_debug_ctx_handler_api(ctx, parsed_api);
        }
    }
}

#endif

void windbg_debug_parser_hook(bool is_kernel, const uint8_t *buf, int len)
{
 #if (ENABLE_PARSER)
    if (is_kernel) {
 # if (ENABLE_KERNEL_PARSER)
        static ParsingContext ctx = {
            .state = STATE_LEADER,
            .result = RESULT_NONE,
            .name = "Kernel"
        };
        windbg_debug_parser(&ctx, buf, len);
 # endif
    }
    else {
 # if (ENABLE_WINDBG_PARSER)
        static ParsingContext ctx = {
            .state = STATE_LEADER,
            .result = RESULT_NONE,
            .name = "WinDbg"
        };
        windbg_debug_parser(&ctx, buf, len);
 # endif
    }
 #endif
}

void windbg_start_sync(void)
{
    if (windbg_state && !windbg_state->is_loaded) {
        windbg_state->is_loaded = windbg_on_load();
    }
}

static void windbg_exit(void)
{
    windbg_on_exit();
 #if (WINDBG_DEBUG_ON)
    FCLOSE(windbg_state->dump_file);
 #endif
 #if (ENABLE_PARSER)
    FCLOSE(parsed_packets);
    FCLOSE(parsed_api);
 #endif
    g_free(windbg_state);
}

int windbg_start(const char *device)
{
    if (windbg_state) {
        WINDBG_ERROR("Multiple instances are not supported");
        exit(1);
    }

    windbg_state = g_malloc0(sizeof(WindbgState));
    windbg_state->ctrl_packet_id = RESET_PACKET_ID;
    windbg_state->data_packet_id = INITIAL_PACKET_ID;

    if (!register_excp_debug_handler(windbg_bp_handler)) {
        WINDBG_ERROR("Another debugger stub has already been registered");
        exit(1);
    }

    // open external pipe for listening to windbg
    windbg_state->chr = qemu_chr_new(WINDBG, device, NULL);
    if (!windbg_state->chr) {
        return -1;
    }

    qemu_chr_fe_claim_no_fail(windbg_state->chr);
    qemu_chr_add_handlers(windbg_state->chr, windbg_chr_can_receive,
                          windbg_chr_receive, NULL, NULL);

 #if (WINDBG_DEBUG_ON)
    windbg_state->dump_file = fopen(WINDBG_DIR "dump.txt", "wb");
 #endif
 #if (ENABLE_PARSER)
    windbg_debug_open_files();
 #endif

    atexit(windbg_exit);
    return 0;
}