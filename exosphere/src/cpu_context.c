#include <stdbool.h>
#include <stdint.h>
#include "arm.h"
#include "cpu_context.h"
#include "flow.h"
#include "pmc.h"
#include "timers.h"
#include "smc_api.h"
#include "utils.h"
#include "synchronization.h"
#include "preprocessor.h"

#define SAVE_SYSREG64(reg) do { __asm__ __volatile__ ("mrs %0, " #reg : "=r"(temp_reg) :: "memory"); g_cpu_contexts[current_core].reg = temp_reg; } while(false)
#define SAVE_SYSREG32(reg) do { __asm__ __volatile__ ("mrs %0, " #reg : "=r"(temp_reg) :: "memory"); g_cpu_contexts[current_core].reg = (uint32_t)temp_reg; } while(false)
#define SAVE_BP_REG(i, _) SAVE_SYSREG64(DBGBVR##i##_EL1); SAVE_SYSREG64(DBGBCR##i##_EL1);
#define SAVE_WP_REG(i, _) SAVE_SYSREG64(DBGBVR##i##_EL1); SAVE_SYSREG64(DBGBCR##i##_EL1);

#define RESTORE_SYSREG64(reg) do { temp_reg = g_cpu_contexts[current_core].reg; __asm__ __volatile__ ("msr " #reg ", %0" :: "r"(temp_reg) : "memory"); } while(false)
#define RESTORE_SYSREG32(reg) RESTORE_SYSREG64(reg)
#define RESTORE_BP_REG(i, _) RESTORE_SYSREG64(DBGBVR##i##_EL1); RESTORE_SYSREG64(DBGBCR##i##_EL1);
#define RESTORE_WP_REG(i, _) RESTORE_SYSREG64(DBGBVR##i##_EL1); RESTORE_SYSREG64(DBGBCR##i##_EL1);

/* start.s */
void __attribute__((noreturn)) __jump_to_lower_el(uint64_t arg, uintptr_t ep, unsigned int el);

/* See notes in start.s */
critical_section_t g_boot_critical_section = {{{.ticket_number = 1}}};

static saved_cpu_context_t g_cpu_contexts[NUM_CPU_CORES] = {0};

void use_core_entrypoint_and_argument(uint32_t core, uintptr_t *entrypoint_addr, uint64_t *argument) {
    saved_cpu_context_t *ctx = &g_cpu_contexts[core];
    if(ctx->ELR_EL3 == 0 || ctx->is_active) {
        panic(0xF7F00007); /* invalid context */
    }

    *entrypoint_addr = ctx->ELR_EL3;
    *argument = ctx->argument;

    ctx->ELR_EL3 = 0;
    ctx->argument = 0;
    ctx->is_active = 1;
}

void set_core_entrypoint_and_argument(uint32_t core, uintptr_t entrypoint_addr, uint64_t argument) {
    g_cpu_contexts[core].ELR_EL3 = entrypoint_addr;
    g_cpu_contexts[core].argument = argument;
}

void __attribute__((noreturn)) core_jump_to_lower_el(void) {
    uintptr_t ep;
    uint64_t arg;
    unsigned int core_id = get_core_id();

    use_core_entrypoint_and_argument(core_id, &ep, &arg);
    critical_section_leave(&g_boot_critical_section);
    flush_dcache_range(&g_boot_critical_section, (uint8_t *)&g_boot_critical_section + sizeof(g_boot_critical_section)); /* already does a dsb sy */
    __sev();

    /* Nintendo jumps to EL1, we jump to EL2. Both are supported with all current packages2. */
    __jump_to_lower_el(arg, ep, 2);
}

uint32_t cpu_on(uint32_t core, uintptr_t entrypoint_addr, uint64_t argument) {
    /* Is core valid? */
    if (core >= NUM_CPU_CORES) {
        return 0xFFFFFFFE;
    }
    
    /* Is core already on? */
    if (g_cpu_contexts[core].is_active) {
        return 0xFFFFFFFC;
    }

    set_core_entrypoint_and_argument(core, entrypoint_addr, argument);

    const uint32_t status_masks[NUM_CPU_CORES] = {0x4000, 0x200, 0x400, 0x800};
    const uint32_t toggle_vals[NUM_CPU_CORES] = {0xE, 0x9, 0xA, 0xB};
    
    /* Check if we're already in the correct state. */
    if ((APBDEV_PMC_PWRGATE_STATUS_0 & status_masks[core]) != status_masks[core]) {
        uint32_t counter = 5001;
        
        /* Poll the start bit until 0 */
        while (APBDEV_PMC_PWRGATE_TOGGLE_0 & 0x100) {
            wait(1);
            counter--;
            if (counter < 1) {
                return 0;
            }
        }
        
        /* Program PWRGATE_TOGGLE with the START bit set to 1, selecting CE[N] */
        APBDEV_PMC_PWRGATE_TOGGLE_0 = toggle_vals[core] | 0x100;
        
        /* Poll until we're in the correct state. */
        counter = 5001;
        while (counter > 0) {
            if ((APBDEV_PMC_PWRGATE_STATUS_0 & status_masks[core]) == status_masks[core]) {
                break;
            }
            wait(1);
            counter--;
        }
    }
    return 0;
}

void power_down_current_core(void) {
    unsigned int current_core = get_core_id();
    flow_set_csr(current_core, 0);
    flow_set_halt_events(current_core, false);
    flow_set_cc4_ctrl(current_core, 0);
    save_current_core_context();
    g_cpu_contexts[current_core].is_active = 0;
    flush_dcache_all();
    finalize_powerdown();
}

uint32_t cpu_off(void) {
    unsigned int current_core = get_core_id();
    if (current_core == 3) {
        power_down_current_core();
    } else {
        clear_priv_smc_in_progress();
        call_with_stack_pointer(get_exception_entry_stack_address(current_core), power_down_current_core);
    }
    while (true) {
        /* Wait forever. */
    }
    return 0;
}

void save_current_core_context(void) {
    unsigned int current_core = get_core_id();
    uint64_t temp_reg = 1;
    /* Write 1 to OS lock .*/
    __asm__ __volatile__ ("msr oslar_el1, %0" : : "r"(temp_reg));

    /* Save system registers. */

    SAVE_SYSREG32(OSDTRRX_EL1);
    SAVE_SYSREG32(MDSCR_EL1);
    SAVE_SYSREG32(OSECCR_EL1);
    SAVE_SYSREG32(MDCCINT_EL1);
    SAVE_SYSREG32(DBGCLAIMCLR_EL1);
    SAVE_SYSREG32(DBGVCR32_EL2);
    SAVE_SYSREG32(SDER32_EL3);
    SAVE_SYSREG32(MDCR_EL2);
    SAVE_SYSREG32(MDCR_EL3);

    EVAL(REPEAT(6, SAVE_BP_REG, ~));
    EVAL(REPEAT(4, SAVE_WP_REG, ~));

    /* Mark context as saved. */
    g_cpu_contexts[current_core].is_saved = 1;
}

void restore_current_core_context(void) {
    unsigned int current_core = get_core_id();
    uint64_t temp_reg;

    if (g_cpu_contexts[current_core].is_saved) {
        RESTORE_SYSREG32(OSDTRRX_EL1);
        RESTORE_SYSREG32(MDSCR_EL1);
        RESTORE_SYSREG32(OSECCR_EL1);
        RESTORE_SYSREG32(MDCCINT_EL1);
        RESTORE_SYSREG32(DBGCLAIMCLR_EL1);
        RESTORE_SYSREG32(DBGVCR32_EL2);
        RESTORE_SYSREG32(SDER32_EL3);
        RESTORE_SYSREG32(MDCR_EL2);
        RESTORE_SYSREG32(MDCR_EL3);

        EVAL(REPEAT(6, RESTORE_BP_REG, ~));
        EVAL(REPEAT(4, RESTORE_WP_REG, ~));

        g_cpu_contexts[current_core].is_saved = 0;
    }
}

bool is_core_active(uint32_t core) {
    return g_cpu_contexts[core].is_active != 0;
}

void set_core_is_active(uint32_t core, bool is_active) {
    g_cpu_contexts[core].is_active = (is_active) ? 1 : 0;
}

void set_current_core_active(void) {
    set_core_is_active(get_core_id(), true);
}

void set_current_core_inactive(void) {
    set_core_is_active(get_core_id(), false);
}

