typedef unsigned int uint32_t;

#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)

#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL_CYCCNTENA 1u
#define SYST_CSR_ENABLE 1u
#define SYST_CSR_TICKINT (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2)

extern unsigned long _estack;
extern unsigned long __bss_start__;
extern unsigned long __bss_end__;

static volatile uint32_t active_sink;
static volatile uint32_t systick_count;

static void semihost_write0(const char *text)
{
    register uint32_t r0 __asm("r0") = 0x04u;
    register const char *r1 __asm("r1") = text;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
}

static void semihost_exit(int code)
{
    uint32_t args[2];
    register uint32_t r0 __asm("r0") = 0x20u;
    register uint32_t r1 __asm("r1");

    args[0] = 0x20026u;
    args[1] = (uint32_t)code;
    r1 = (uint32_t)args;
    __asm volatile("bkpt 0xab" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {
    }
}

static void write_u32_dec(uint32_t value)
{
    char buffer[11];
    char *p = buffer + sizeof(buffer);

    *--p = '\0';
    if (value == 0u) {
        *--p = '0';
    } else {
        while (value != 0u) {
            *--p = (char)('0' + (value % 10u));
            value /= 10u;
        }
    }
    semihost_write0(p);
}

static void write_kv_u32(const char *key, uint32_t value)
{
    semihost_write0(key);
    semihost_write0("=");
    write_u32_dec(value);
    semihost_write0("\n");
}

static void dwt_reset(void)
{
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0u;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}

static uint32_t run_active_probe(void)
{
    uint32_t i;

    dwt_reset();
    for (i = 0; i < 200000u; i++) {
        active_sink = (active_sink * 33u) ^ (i + 0x9e3779b9u);
    }
    return DWT_CYCCNT;
}

static uint32_t run_wfi_probe(void)
{
    uint32_t cycles;

    systick_count = 0u;
    SYST_RVR = 1000u;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
    dwt_reset();
    __asm volatile("cpsie i" : : : "memory");
    __asm volatile("wfi" : : : "memory");
    cycles = DWT_CYCCNT;
    SYST_CSR = 0u;
    return cycles;
}

void Reset_Handler(void)
{
    unsigned long *p;
    uint32_t active_cycles;
    uint32_t wfi_cycles;

    for (p = &__bss_start__; p < &__bss_end__; p++) {
        *p = 0u;
    }

    semihost_write0("sh264e_sim_profile_probe=1\n");
    semihost_write0("target=cortex-m7\n");
    active_cycles = run_active_probe();
    write_kv_u32("active_cycles", active_cycles);
    write_kv_u32("active_sink", active_sink);

    semihost_write0("wfi_probe=begin\n");
    wfi_cycles = run_wfi_probe();
    semihost_write0("wfi_probe=end\n");
    write_kv_u32("wfi_cycles", wfi_cycles);
    write_kv_u32("systick_count", systick_count);

    semihost_exit(0);
}

void SysTick_Handler(void)
{
    systick_count++;
}

void Default_Handler(void)
{
    semihost_exit(99);
}

__attribute__((section(".isr_vector"), used))
void (*const g_vector_table[])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler,
    Default_Handler,
    Default_Handler,
    Default_Handler,
    Default_Handler,
    0,
    0,
    0,
    0,
    Default_Handler,
    Default_Handler,
    0,
    Default_Handler,
    SysTick_Handler,
};
