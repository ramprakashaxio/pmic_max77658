/*
 * max77658_main.c - Application Specific PMIC Logic
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <math.h>

#include "max77658_main.h"
#include "data_manager.h"
#include "bsp.h"
#include "pmic_gpio.h"
#include "max77658_pm.h"
#include "max77658_fg.h"
#include "max77658_defines.h"
#include "app_i2c_lock.h"

LOG_MODULE_REGISTER(pmic_app, CONFIG_LOG_DEFAULT_LEVEL);

/* --- Shutdown Request Coordination --- */
static atomic_t g_shutdown_req = ATOMIC_INIT(0);
static atomic_t g_shutdown_in_progress = ATOMIC_INIT(0);

bool max77658_shutdown_requested(void)
{
    return atomic_get(&g_shutdown_req) != 0;
}

void max77658_request_software_off(const char *reason)
{
    if (!atomic_cas(&g_shutdown_req, 0, 1)) {
        return; // already requested
    }
    LOG_WRN("PMIC shutdown requested: %s", reason ? reason : "(no reason)");
}

/* --- PMIC Thread Configuration --- */
#define PMIC_STACK_SIZE 1024
#define PMIC_PRIORITY   7

K_THREAD_STACK_DEFINE(pmic_stack_area, PMIC_STACK_SIZE);
struct k_thread pmic_thread_data;

/* --- Configuration Constants (60mAh Battery) --- */
#define BAT_CAP_MAH         60
#define BAT_TERM_CURR_MA    6
#define BAT_V_EMPTY_MV      3200
#define BAT_V_CHARGE_MV     4200
#define BAT_RSENSE_MOHM     50

/* --- Smart Power Cutoff --- */
#define VBAT_CUTOFF_MV      3200  /* Shutdown threshold */
#define VBAT_DB_CNT         4     /* 4 * 2s = 8s debounce */

/* --- Helper Functions: Convert Human Units to Register Values --- */

/**
 * @brief Convert battery capacity (mAh) to DesignCap register value
 * @param cap_mAh Capacity in milliamp-hours
 * @param rsense_mohm Sense resistor in milliohms
 * @return Register value for DESIGNCAP_REG
 * 
 * DesignCap LSB = 5.0µVh / Rsense
 * For 10mΩ: LSB = 0.5mAh per bit → raw = cap / 0.5 = cap * 2
 * Example: 60mAh → 60 * 10 / 5 = 120 (0x0078)
 */
static inline uint16_t fg_designcap_reg(int cap_mAh, int rsense_mohm)
{
    return (uint16_t)((cap_mAh * rsense_mohm) / 5);
}

/**
 * @brief Convert termination current (mA) to IChgTerm register value
 * @param iterm_mA Termination current in milliamps
 * @param rsense_mohm Sense resistor in milliohms
 * @return Register value for ICHGTERM_REG
 * 
 * Current LSB = 1.5625µV / Rsense
 * For 10mΩ: LSB = 156.25µA per bit
 * Example: 6mA → (6 * 1000) / 156.25 ≈ 38 (0x0026)
 */
static inline uint16_t fg_ichgterm_reg(int iterm_mA, int rsense_mohm)
{
    float raw = ((float)iterm_mA * 1000.0f) / (1562.5f / (float)rsense_mohm);
    return (uint16_t)(raw + 0.5f);
}

/**
 * @brief Encode VEmpty register (empty voltage + recovery voltage)
 * @param ve_mV Empty voltage threshold in millivolts
 * @param vr_mV Recovery voltage in millivolts
 * @return Register value for VEMPTY_REG
 * 
 * VEmpty[15:7] = VE (empty voltage): LSB = 10mV
 * VEmpty[6:0] = VR (recovery voltage): LSB = 40mV
 * Example: 3200mV/3600mV → 0xA05A
 */
static inline uint16_t fg_vempty_reg(int ve_mV, int vr_mV)
{
    uint16_t ve = (uint16_t)((ve_mV / 10) & 0x01FF);
    uint16_t vr = (uint16_t)((vr_mV / 40) & 0x007F);
    return (uint16_t)((ve << 7) | vr);
}

/* --- Data Structures --- */
static max77658_pm_t pm_ctx;
static max77658_fg_t fg_ctx;

static struct {
    bool fg_ready;
    volatile bool irq_pending;
    uint8_t vbat_cutoff_cnt;  /* Debounce counter for low voltage */
} app_state = { false, false, 0 };

/* --- Internal Helpers --- */

/* Interrupt Callback (Runs in ISR context) */
static void internal_irq_handler(void *user_data)
{
    ARG_UNUSED(user_data);
    app_state.irq_pending = true;
}

static int sbb_reg_to_mv(uint8_t reg_val)
{
    return 500 + (reg_val * 25);
}

/**
 * @brief Check if USB charger is connected and valid
 * @return true if CHGIN is valid (not in overvoltage/undervoltage)
 */
static bool max77658_is_charger_connected(void)
{
    uint8_t stat_chg_b;
    int ret = bsp_i2c_reg_read(MAX77658_PM_ADDR, MAX77658_STAT_CHG_B, &stat_chg_b, 1);
    if (ret != 0) {
        return false; /* I2C error, assume not connected */
    }
    
    /* CHGIN_DTLS[3:2]: 0b11 = Valid input */
    uint8_t chgin_dtls = (stat_chg_b >> 2) & 0x03;
    return (chgin_dtls == 3);
}

/**
 * @brief Smart power management: debounced low voltage cutoff
 * 
 * Uses average cell voltage (immune to pulse artifacts) with 8-second
 * debounce to prevent shutdown during brief dips. Only shuts down if
 * charger is not connected.
 */
static void max77658_smart_power_check(void)
{
    if (!app_state.fg_ready) {
        return;
    }

    /* Use AvgVCell for stability (filters out transient dips from PPG pulses) */
    double vavg_uv = max77658_fg_get_avgVcell(&fg_ctx);
    int vavg_mv = (int)(vavg_uv / 1000.0);
    
    /* Read SOC and current for logging */
    int soc = max77658_fg_get_SOC(&fg_ctx);
    float current_uA = max77658_fg_get_Current(&fg_ctx);
    float current_mA = current_uA / 1000.0f;
    
    uint8_t chg_present = max77658_is_charger_connected() ? 1 : 0;

    /* current in 0.1mA (signed) with clamp */
    float i_x10_f = current_mA * 10.0f;
    if (i_x10_f >  32767.0f) i_x10_f =  32767.0f;
    if (i_x10_f < -32768.0f) i_x10_f = -32768.0f;
    int16_t batt_ma_x10 = (int16_t)i_x10_f;

    data_manager_update_batt((uint8_t)soc, (uint16_t)vavg_mv, batt_ma_x10, chg_present, true);
    
    LOG_INF("Battery: %d%% | %d mV (avg) | %.2f mA", soc, vavg_mv, (double)current_mA);

    /* Cutoff logic: only if NOT charging and voltage is critically low */
    if (!max77658_is_charger_connected() && vavg_mv <= VBAT_CUTOFF_MV) {
        app_state.vbat_cutoff_cnt++;
        LOG_WRN("Low battery: %d mV (count: %d/%d)", vavg_mv, 
                app_state.vbat_cutoff_cnt, VBAT_DB_CNT);
        
        if (app_state.vbat_cutoff_cnt >= VBAT_DB_CNT) {
            LOG_ERR("!!! CRITICAL LOW BATTERY - ENTERING SHIP MODE !!!");
            max77658_request_software_off("Battery voltage cutoff");
        }
    } else {
        /* Reset counter if voltage recovers or charger connected */
        if (app_state.vbat_cutoff_cnt > 0) {
            LOG_INF("Voltage recovered or charging - reset cutoff counter");
        }
        app_state.vbat_cutoff_cnt = 0;
    }
}

/**
 * @brief Diagnostic: Decode raw current register for Rsense calibration
 * 
 * Logs raw CURRENT and AVGCURRENT register values and computes the resulting
 * current in mA for three common Rsense values (10mΩ, 50mΩ, 200mΩ).
 * 
 * Compare these values against your bench supply or DMM reading:
 * - If I@10m matches your meter → set BAT_RSENSE_MOHM = 10
 * - If I@50m matches your meter → set BAT_RSENSE_MOHM = 50
 * - If I@200m matches your meter → set BAT_RSENSE_MOHM = 200
 * 
 * IMPORTANT: After changing BAT_RSENSE_MOHM, you MUST rebuild to update:
 * - fg_designcap_reg() calculation
 * - fg_ichgterm_reg() calculation
 * - All capacity/current conversion functions
 */
static void fg_dump_current_raw(max77658_fg_t *fg)
{
    uint16_t raw_u16 = 0, raw_avg_u16 = 0;
    int ret;

    /* Read raw register values */
    ret = max77658_fg_read_reg(fg, CURRENT_REG, &raw_u16);
    if (ret != 0) {
        LOG_ERR("Failed to read CURRENT_REG");
        return;
    }
    
    ret = max77658_fg_read_reg(fg, AVGCURRENT_REG, &raw_avg_u16);
    if (ret != 0) {
        LOG_ERR("Failed to read AVGCURRENT_REG");
        return;
    }

    /* Treat as signed 16-bit (2's complement) */
    int16_t raw = (int16_t)raw_u16;
    int16_t raw_avg = (int16_t)raw_avg_u16;

    /* Compute current for different Rsense values */
    /* Formula: I(mA) = raw × (1562.5 µV / Rsense_mΩ) / 1000 */
    float i10  = ((float)raw * 1562.5f) / 10.0f  / 1000.0f;   // Rsense = 10mΩ
    float i50  = ((float)raw * 1562.5f) / 50.0f  / 1000.0f;   // Rsense = 50mΩ
    float i200 = ((float)raw * 1562.5f) / 200.0f / 1000.0f;  // Rsense = 200mΩ

    LOG_INF("=== RSENSE CALIBRATION ===");
    LOG_INF("FG Inst Current: raw=0x%04X (%d decimal)", raw_u16, raw);
    LOG_INF("  @ 10mΩ:  %.2f mA", (double)i10);
    LOG_INF("  @ 50mΩ:  %.2f mA", (double)i50);
    LOG_INF("  @ 200mΩ: %.2f mA", (double)i200);

    float a10  = ((float)raw_avg * 1562.5f) / 10.0f  / 1000.0f;
    float a50  = ((float)raw_avg * 1562.5f) / 50.0f  / 1000.0f;
    float a200 = ((float)raw_avg * 1562.5f) / 200.0f / 1000.0f;

    LOG_INF("FG Avg Current:  raw=0x%04X (%d decimal)", raw_avg_u16, raw_avg);
    LOG_INF("  @ 10mΩ:  %.2f mA", (double)a10);
    LOG_INF("  @ 50mΩ:  %.2f mA", (double)a50);
    LOG_INF("  @ 200mΩ: %.2f mA", (double)a200);
    LOG_INF("Compare against your DMM/bench supply reading!");
    LOG_INF("==========================");
}

/* Log System Status (Voltage, Current, Errors) */
static void log_status(void)
{
    int32_t ret;
    
    LOG_INF("--- System Status Report ---");

    /* 1. Error Flags */
    ret = max77658_pm_get_ERCFLAG(&pm_ctx);
    if (ret > 0) {
        LOG_WRN("ERC Flags: 0x%02X", ret);
        if (ret & 0x04) LOG_ERR(" >> SYSUVLO DETECTED (Brownout!)");
    }

    /* 2. Charger Status */
    ret = max77658_pm_get_CHG_DTLS(&pm_ctx);
    if (ret >= 0) LOG_INF("Charger State: 0x%X", ret);

    /* 3. Fuel Gauge - delegated to smart_power_check() */
    max77658_smart_power_check();
    
    /* 3.1. Rsense Calibration Diagnostic (compare against bench supply) */
    if (app_state.fg_ready) {
        fg_dump_current_raw(&fg_ctx);
    }

    /* 4. Rail Voltages */
    int32_t sbb0_ret = max77658_pm_get_TV_SBB0(&pm_ctx);
    int32_t sbb1_ret = max77658_pm_get_TV_SBB1(&pm_ctx);
    int32_t sbb2_ret = max77658_pm_get_TV_SBB2(&pm_ctx);
    
    if (sbb0_ret >= 0 && sbb1_ret >= 0 && sbb2_ret >= 0) {
        LOG_INF("Rails: SBB0=%dmV, SBB1=%dmV, SBB2=%dmV", 
                sbb_reg_to_mv((uint8_t)sbb0_ret), 
                sbb_reg_to_mv((uint8_t)sbb1_ret), 
                sbb_reg_to_mv((uint8_t)sbb2_ret));
    } else {
        LOG_ERR("Failed to read rail voltages (SBB0=%d, SBB1=%d, SBB2=%d)", 
                sbb0_ret, sbb1_ret, sbb2_ret);
    }
}

static void process_interrupt(void)
{
    int32_t ret;
    uint8_t int0, int1;

    LOG_INF(">> Processing Interrupt...");

    /* Read/Clear INT_GLBL0 */
    ret = max77658_pm_get_INT_GLBL0(&pm_ctx);
    if (ret >= 0) {
        int0 = (uint8_t)ret;
        if (int0 & 0x04) LOG_INF("   nEN Button Pressed");
        if (int0 & 0x08) LOG_INF("   nEN Button Released");
    }

    /* Read/Clear INT_GLBL1 */
    ret = max77658_pm_get_INT_GLBL1(&pm_ctx);
    if (ret >= 0) {
        int1 = (uint8_t)ret;
        if (int1 & 0x04) LOG_ERR("   SBB Regulator Timeout/Fault");
    }
}

/* -------------------------------------------------------------------------- */
/* Power Rail Configuration                                                   */
/* -------------------------------------------------------------------------- */

#define REG_CNFG_SBB0_B  0x3A

static int configure_pmic_regulators(void)
{
    int32_t ret;
    uint8_t val;
    int errors = 0;

    LOG_INF("=== Configuring Power Rails (SBB0=1.8V, SBB1=3.3V, SBB2=5.0V) ===");

    /* --------------------------------------------------------- */
    /* GLOBAL SETTINGS: Enable 1.5A Peak Current                 */
    /* --------------------------------------------------------- */
    ret = max77658_pm_set_IPK_1P5A(&pm_ctx, 1);
    if (ret < 0) LOG_ERR("Failed to set 1.5A Limit bit");

    /* --------------------------------------------------------- */
    /* 1. Configure SBB0 (1.8V) - KEEPING YOUR WORKING FIX       */
    /* --------------------------------------------------------- */
    /* A. Set Main Target to 1.8V (0x34) */
    ret = max77658_pm_set_TV_SBB0(&pm_ctx, 0x34);
    if (ret < 0) errors++;

    /* B. Set DVS Target to 1.8V (0x34) */
    ret = max77658_pm_set_TV_SBB0_DVS(&pm_ctx, 0x34);
    if (ret < 0) errors++;

    /* C. Force Buck Mode (01) & Force Enable (110) */
    ret = bsp_i2c_reg_read(MAX77658_PM_ADDR, REG_CNFG_SBB0_B, &val, 1);
    if (ret == 0) {
        val &= ~(0xF7);         // Clear bits
        val |= (0x01 << 6);     // OP_MODE = Buck
        val |= (0x00 << 4);     // IP = 1.0A
        val |= 0x06;            // EN = Force On
        bsp_i2c_reg_write(MAX77658_PM_ADDR, REG_CNFG_SBB0_B, &val, 1);
    } else { errors++; }

    /* --------------------------------------------------------- */
    /* 2. Configure SBB1 (3.3V)                                  */
    /* --------------------------------------------------------- */
    max77658_pm_set_IP_SBB1(&pm_ctx, 0); 
    max77658_pm_set_TV_SBB1(&pm_ctx, 0x70); 
    max77658_pm_set_EN_SBB1(&pm_ctx, 0x06); 

    /* --------------------------------------------------------- */
    /* 3. Configure SBB2 (5.0V) - THE FIX                        */
    /* --------------------------------------------------------- */
    /* Set Peak Current to Max (Combined with Global 1.5A) */
    max77658_pm_set_IP_SBB2(&pm_ctx, 0); // 0b00

    /* Ramp Sequence to 5.0V */
    /* 1. Start at 3.3V (0x70) */
    max77658_pm_set_TV_SBB2(&pm_ctx, 0x70); 
    max77658_pm_set_EN_SBB2(&pm_ctx, 0x0E); // Force Enable (preserve defaults)
    k_msleep(20);

    /* 2. Ramp to 4.2V (0x94) */
    max77658_pm_set_TV_SBB2(&pm_ctx, 0x94); 
    k_msleep(20);

    /* 3. Target 5.0V (0xB4) - CHANGED FROM 0x7A */
    /* Formula: (5.0 - 0.5) / 0.025 = 180 = 0xB4 */
    ret = max77658_pm_set_TV_SBB2(&pm_ctx, 0xB4); 
    
    if (ret < 0) {
        LOG_ERR("Failed to set SBB2 to 5.0V");
        errors++;
    } else {
        LOG_INF("SBB2 Configured: 5.0V @ 1.5A Peak Limit");
    }

    /* --------------------------------------------------------- */
    /* 4. Disable LDOs                                           */
    /* --------------------------------------------------------- */
    max77658_pm_set_EN_LDO0(&pm_ctx, 0x04);
    max77658_pm_set_EN_LDO1(&pm_ctx, 0x04);

    return (errors == 0) ? 0 : -1;
}

static void debug_dump_all_rail_registers(void)
{
    uint8_t sbb0_tv, sbb0_dvs, sbb0_cfg, sbb1_tv, sbb1_cfg, sbb2_tv, sbb2_cfg;
    
    /* Read SBB0 registers */
    bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x39, &sbb0_tv, 1);   // TV_SBB0
    bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x3F, &sbb0_dvs, 1);  // DVS_SBB0
    bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x3A, &sbb0_cfg, 1);  // CNFG_SBB0_B
    
    /* Read SBB1 registers */
    bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x3B, &sbb1_tv, 1);   // TV_SBB1
    bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x3C, &sbb1_cfg, 1);  // CNFG_SBB1_B
    
    /* Read SBB2 registers */
    bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x3D, &sbb2_tv, 1);   // TV_SBB2
    bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x3E, &sbb2_cfg, 1);  // CNFG_SBB2_B

    LOG_INF("=== VOLTAGE REGISTER VERIFICATION ===");
    LOG_INF("SBB0 (Target: 1.8V = 0x34):");
    LOG_INF("  TV_SBB0 [0x39]:  0x%02X = %d mV", sbb0_tv, sbb_reg_to_mv(sbb0_tv));
    LOG_INF("  DVS_SBB0 [0x3F]: 0x%02X = %d mV", sbb0_dvs, sbb_reg_to_mv(sbb0_dvs));
    LOG_INF("  CNFG [0x3A]:     0x%02X (Mode bits: 0x%02X)", sbb0_cfg, (sbb0_cfg >> 6) & 0x03);
    
    LOG_INF("SBB1 (Target: 3.3V = 0x70):");
    LOG_INF("  TV_SBB1 [0x3B]:  0x%02X = %d mV", sbb1_tv, sbb_reg_to_mv(sbb1_tv));
    LOG_INF("  CNFG [0x3C]:     0x%02X", sbb1_cfg);
    
    LOG_INF("SBB2 (Target: 5.0V = 0xB4):");
    LOG_INF("  TV_SBB2 [0x3D]:  0x%02X = %d mV", sbb2_tv, sbb_reg_to_mv(sbb2_tv));
    LOG_INF("  CNFG [0x3E]:     0x%02X", sbb2_cfg);
    LOG_INF("=====================================");
}

/* -------------------------------------------------------------------------- */
/* Main Initialization Logic                                                  */
/* -------------------------------------------------------------------------- */

int max77658_app_init(void)
{
    int ret;
    uint8_t val;

    LOG_INF("==============================================");
    LOG_INF("   Initializing MAX77658 Subsystem");
    LOG_INF("==============================================");

    /* 1. Low Level BSP (I2C) */
    if (bsp_init() != 0) {
        LOG_ERR("BSP Init Failed");
        return -1;
    }

    /* Setup PMIC context early for diagnostics */
    pm_ctx.device_address = MAX77658_PM_ADDR;
    pm_ctx.read_reg = bsp_i2c_reg_read;
    pm_ctx.write_reg = bsp_i2c_reg_write;

    /* --------------------------------------------------------------- */
    /* PATCH: Disable LDO1 Active Discharge ASAP (ADE_LDO1 = bit3)      */
    /* CNFG_LDO1_B = 0x4B. This prevents the ~100 ohm discharge burn.   */
    /* --------------------------------------------------------------- */
    ret = bsp_i2c_reg_read(MAX77658_PM_ADDR, 0x4B, &val, 1);
    if (ret == 0) {
        val &= ~(1U << 3); /* ADE_LDO1 = 0 */
        (void)bsp_i2c_reg_write(MAX77658_PM_ADDR, 0x4B, &val, 1);
        LOG_INF("LDO1 Active Discharge Disabled (ADE_LDO1=0)");
    } else {
        LOG_WRN("Failed to disable ADE_LDO1 (ret=%d)", ret);
    }

    /* 2. GPIO & Hardware Reset */
    if (pmic_gpio_init() == 0) {
        /* Diagnostic: Read raw GPIO pin states */
        LOG_INF("GPIO raw: nEN=%d nRST=%d nIRQ=%d",
                pmic_gpio_get_nen(),
                pmic_gpio_get_nrst(),
                pmic_gpio_get_nirq());

        pmic_irq_register_callback(internal_irq_handler, NULL);
        pmic_irq_enable();

        /* CHARGER-ONLY BOOT POLICY: Check CHGIN validity FIRST */
        /* Wait for PMIC to wake, but limit attempts to prevent infinite loop */
        LOG_INF("Waiting for PMIC to wake (charger insertion required)...");
        int wake_attempts = 0;
        while (wake_attempts < 50) {  /* 50 * 200ms = 10 second timeout */
            int cid = max77658_pm_get_CID(&pm_ctx);
            if (cid >= 0) {
                LOG_INF("PMIC woke up! (CID=0x%02X)", cid);
                break;
            }
            LOG_WRN("PMIC asleep. Plug charger (CHGIN)...");
            k_msleep(200);
            wake_attempts++;
        }

        /* Early CHGIN validity check - do this BEFORE full init */
        uint8_t stat_chg_b_early;
        ret = bsp_i2c_reg_read(MAX77658_PM_ADDR, MAX77658_STAT_CHG_B, &stat_chg_b_early, 1);
        if (ret == 0) {
            uint8_t chgin_dtls_early = (stat_chg_b_early >> 2) & 0x03;
            LOG_INF("Early CHGIN check: STAT_CHG_B=0x%02X, CHGIN_DTLS=%u", stat_chg_b_early, chgin_dtls_early);
            
            if (chgin_dtls_early != 3) {
                LOG_WRN("Boot without valid charger: entering SHIP MODE immediately (prevent boot loop)");
                pmic_irq_disable();
                pmic_nen_release_hiz();
                max77658_pm_set_SFT_CTRL_novfy(&pm_ctx, 0x03);
                k_msleep(200);
                while (1) { k_msleep(1000); }
            }
            LOG_INF("CHGIN valid - continuing boot");
        } else {
            LOG_ERR("Failed to read STAT_CHG_B early check (I2C error)");
        }

        /* Verify PMIC is accessible and ready for configuration */
        ret = max77658_pm_get_CID(&pm_ctx);
        if (ret < 0) {
            LOG_ERR("PMIC Not Found (I2C Error)");
            return -1;
        }
        LOG_INF("PMIC Connected (ID: 0x%02X)", ret);

        /* nRST is only a debug signal - log state but don't gate init on it */
        LOG_WRN("nRST raw=%d (ignored for readiness)", pmic_gpio_get_nrst());
    } else {
        LOG_WRN("PMIC GPIO Init Failed (Running without IRQ)");
    }

    /* 3. Setup Fuel Gauge Context */
    fg_ctx.device_address = 0x36;
    fg_ctx.read_reg = bsp_i2c_reg_read;
    fg_ctx.write_reg = bsp_i2c_reg_write;

    /* --- BOOT WAKE SOURCE DETECTION --- */
    uint8_t stat_chg_b, stat_glbl, ercflag;
    bsp_i2c_reg_read(MAX77658_PM_ADDR, MAX77658_STAT_CHG_B, &stat_chg_b, 1);
    bsp_i2c_reg_read(MAX77658_PM_ADDR, MAX77658_STAT_GLBL, &stat_glbl, 1);
    
    ret = max77658_pm_get_ERCFLAG(&pm_ctx);
    ercflag = (ret >= 0) ? (uint8_t)ret : 0;
    
    uint8_t chgin_dtls = (stat_chg_b >> 2) & 0x03;
    
    LOG_INF("=== BOOT WAKE SOURCE DETECTION ===");
    LOG_INF("STAT_CHG_B = 0x%02X (CHGIN_DTLS=%u)", stat_chg_b, chgin_dtls);
    LOG_INF("STAT_GLBL  = 0x%02X", stat_glbl);
    LOG_INF("ERCFLAG    = 0x%02X", ercflag);
    
    if (chgin_dtls == 3) {
        LOG_INF(">>> CHGIN valid (charger present)");
    } else {
        LOG_WRN(">>> CHGIN not valid (dtls=%u) - should not reach here!", chgin_dtls);
    }
    LOG_INF("===================================");

    /* 5. Global Settings */
    max77658_pm_set_nEN_MODE(&pm_ctx, 0x00); // Push-Button Mode
    max77658_pm_set_DBEN_nEN(&pm_ctx, 0x01); // 30ms debounce

    /* 6. Configure Power Rails */
    ret = configure_pmic_regulators();
    if (ret != 0) {
        LOG_WRN("Regulator configuration had errors (continuing)");
    }
    
    /* VERIFY: Dump ALL rail registers to confirm exact voltages */
    debug_dump_all_rail_registers();

    /* 7. Configure Charger for 60mAh Battery */
    /* Target: 0.8C = 48mA, closest setting is 45mA (Code 0x05) */
    max77658_pm_set_CHG_CC(&pm_ctx, 0x05);        /* 45mA charge current */
    max77658_pm_set_CHG_CV(&pm_ctx, 0x18);        /* 4.2V charge voltage */
    max77658_pm_set_I_TERM(&pm_ctx, 0x02);        /* 10% termination (4.5mA) */
    max77658_pm_set_CHG_EN(&pm_ctx, 0x01);        /* Enable charger */

    /* 8. Initialize Fuel Gauge with Register-Coded Values */
    LOG_INF("Initializing Fuel Gauge for 60mAh battery...");
    
    /* CRITICAL: Store register values, not human units! */
    /* config_option_1() writes these directly to hardware registers */
    fg_ctx.pdata.designcap = fg_designcap_reg(BAT_CAP_MAH, BAT_RSENSE_MOHM);   /* 0x0258 (600) for 60mAh @ 50mΩ */
    fg_ctx.pdata.ichgterm  = fg_ichgterm_reg(BAT_TERM_CURR_MA, BAT_RSENSE_MOHM); /* 0x00C0 (192) for 6mA @ 50mΩ */
    fg_ctx.pdata.vempty    = fg_vempty_reg(3200, 3600);  /* 0xA05A (VE=3.2V, VR=3.6V) */
    fg_ctx.pdata.vcharge   = BAT_V_CHARGE_MV;     /* 4200 (used for MODELCFG selection) */
    fg_ctx.pdata.rsense    = BAT_RSENSE_MOHM;     /* 50mΩ */

    /* Diagnostic: Verify computed register values before init */
    LOG_INF("FG Config (Rsense=%dmΩ): DesignCap=0x%04X, DQACC=0x%04X, IChgTerm=0x%04X, VEmpty=0x%04X",
            fg_ctx.pdata.rsense,
            fg_ctx.pdata.designcap,
            (uint16_t)(fg_ctx.pdata.designcap >> 5),
            fg_ctx.pdata.ichgterm,
            fg_ctx.pdata.vempty);

    ret = max77658_fg_init(&fg_ctx);
    if (ret != 0) {
        LOG_WRN("Fuel Gauge Init Warning (%d)", ret);
        app_state.fg_ready = false;
    } else {
        app_state.fg_ready = true;
    }

    LOG_INF("Initialization Complete");
    return 0;
}

void max77658_app_process_events(void)
{
    k_mutex_lock(&i2c_lock, K_FOREVER);

    /* Handle pending IRQs */
    if (app_state.irq_pending) {
        app_state.irq_pending = false;
        process_interrupt();
    }
    
    /* Log system health */
    log_status();
    
    k_mutex_unlock(&i2c_lock);
}

/* -------------------------------------------------------------------------- */
/* PMIC Thread Implementation                                                 */
/* -------------------------------------------------------------------------- */

void pmic_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("PMIC Thread Started");

    while (1) {
        /* If shutdown requested, do it once, cleanly */
        if (atomic_get(&g_shutdown_req) && atomic_cas(&g_shutdown_in_progress, 0, 1)) {

            LOG_WRN(">>> Entering FACTORY SHIP MODE now (wake: CHGIN only)");

            /* 1) Stop IRQ + stop future activity */
            pmic_irq_disable();

            /* 2) Take I2C lock and do final PMIC ops */
            k_mutex_lock(&i2c_lock, K_FOREVER);

            /* Optional: turn off heavy rails BEFORE ship mode (reduces glitches) */
            (void)max77658_pm_set_EN_SBB2(&pm_ctx, 0x04); // disable SBB2
            k_msleep(10);

            /* Keep nEN in Hi-Z (no pull-up) */
            pmic_nen_release_hiz();

            /* 3) Enter ship mode (write-only) */
            max77658_pm_set_SFT_CTRL_novfy(&pm_ctx, 0x03);

            /* not expected to return */
            k_msleep(200);
            while (1) { k_msleep(1000); }
        }

        /* Normal periodic work - Lock I2C bus before talking to hardware */
        k_mutex_lock(&i2c_lock, K_FOREVER);

        /* Check Interrupts */
        if (app_state.irq_pending) {
            app_state.irq_pending = false;
            process_interrupt();
        }
        
        /* Log Status (includes smart power check) */
        log_status();
        
        k_mutex_unlock(&i2c_lock);

        /* Sleep: Update PMIC stats every 2 seconds (0.5Hz) */
        k_msleep(2000);
    }
}

void max77658_app_start(void)
{
    k_thread_create(&pmic_thread_data, pmic_stack_area,
                    K_THREAD_STACK_SIZEOF(pmic_stack_area),
                    pmic_thread_entry,
                    NULL, NULL, NULL,
                    PMIC_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&pmic_thread_data, "pmic_thread");
    LOG_INF("PMIC Thread Created (Priority: %d)", PMIC_PRIORITY);
}

void max77658_enter_software_off(void)
{
    LOG_WRN("Entering SOFTWARE OFF (SFT_OFF=0x02). Wake via CHGIN or nEN per config.");

    k_mutex_lock(&i2c_lock, K_FOREVER);

    pmic_nen_release_hiz();

    // WRITE-ONLY (NO VERIFY!)
    max77658_pm_set_SFT_CTRL_novfy(&pm_ctx, 0x02);

    k_mutex_unlock(&i2c_lock);

    k_msleep(200);
    while (1) { k_msleep(1000); }
}

void max77658_enter_software_off_nolock(void)
{
    LOG_WRN("Entering SOFTWARE OFF (SFT_OFF=0x02). Wake via CHGIN or nEN per config.");

    /* Assumes i2c_lock is already held by caller */
    pmic_nen_release_hiz();

    // WRITE-ONLY (NO VERIFY!)
    max77658_pm_set_SFT_CTRL_novfy(&pm_ctx, 0x02);

    k_msleep(200);
    while (1) { k_msleep(1000); }
}

void max77658_enter_ship_mode(void)
{
    LOG_WRN("Entering FACTORY SHIP MODE (FSM). Wake via CHGIN (charger insertion).");

    /* Stop PMIC IRQ so we don't service anything during collapse */
    pmic_irq_disable();

    /* Take I2C lock for final transactions */
    k_mutex_lock(&i2c_lock, K_FOREVER);

    /* Optional: explicitly disable rails before ship mode (not required, but ok) */
    (void)max77658_pm_set_EN_SBB2(&pm_ctx, 0x04); /* disable */
    (void)max77658_pm_set_EN_SBB1(&pm_ctx, 0x04); /* disable */
    (void)max77658_pm_set_EN_SBB0(&pm_ctx, 0x04); /* disable */
    k_msleep(10);

    /* IMPORTANT: make sure nEN is NOT being driven low by MCU */
    pmic_nen_release_hiz();

    /* Enter ship mode: CNFG_GLBL.SFT_CTRL = 0x03 */
    (void)max77658_pm_set_SFT_CTRL_novfy(&pm_ctx, 0x03);

    k_mutex_unlock(&i2c_lock);

    /* Not expected to return */
    k_msleep(200);
    while (1) { k_msleep(1000); }
}
