/*
 * max77658_fg.c
 *
 * Created on: Nov 9, 2021
 * Author: kai
 * Corrected: [Current Date]
 */

#include "max77658_fg.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "bsp.h" // Assuming bsp_delay_ms is defined here

LOG_MODULE_REGISTER(max77658_fg, CONFIG_PMIC_MAX77658_LOG_LEVEL);

/* Force EZ Config Override (for development/parameter changes) */
#ifndef CONFIG_FG_FORCE_EZ_CONFIG
#define CONFIG_FG_FORCE_EZ_CONFIG 0
#endif

/* POR Mask */
#define MAX17055_POR_MASK               (0xFFFD)
#define MAX17055_CYCLE_MASK             (0x0002)

/* MODELCFG register bits */
#define MAX17055_MODELCFG_REFRESH       (1 << 15)

/* FSTAT register bits */
#define MAX17055_FSTAT_DNR              (1)

/* LIBRARY FUNCTION SUCCESS*/
#define F_SUCCESS_0  0

/* LIBRARY FUNCTION ERROR CODES */
#define F_ERROR_1 -1    //-1 if I2C read/write errors exist
#define F_ERROR_2 -2    //-2 if device is not present
#define F_ERROR_3 -3    //-3 if function error
#define F_ERROR_4 -4    //-4 if other error
#define F_ERROR_5 -5    //-5 if POR not detected

/**
 * @brief      Reads from MAX17055 register.
 */
int32_t max77658_fg_read_reg(max77658_fg_t *ctx, uint8_t reg_addr, uint16_t *value)
{
   int32_t ret;
   uint8_t read_data[2];

   // Read 2 bytes
   ret = ctx->read_reg(ctx->device_address, reg_addr, read_data, 2);
   if(ret == F_SUCCESS_0)
   {
      // MAX17055 sends LSB first, then MSB
      *value = (uint16_t)read_data[0] | ((uint16_t)read_data[1] << 8);
   }

   return ret;
}

/**
 * @brief      Writes a register.
 */
int max77658_fg_write_reg(max77658_fg_t *ctx, uint8_t reg_addr, uint16_t reg_data)
{
   int32_t ret;
   uint8_t buff[2];

   // Send LSB first
   buff[0] = reg_data & 0xFF;
   buff[1] = (reg_data >> 8) & 0xFF;

   ret = ctx->write_reg(ctx->device_address, reg_addr, buff, 2);
   return (ret == 0) ? F_SUCCESS_0 : F_ERROR_1;
}

/**
 * @brief      Reads an specified register from the MAX17055 register.
 */
int16_t max77658_fg_get_regInfo(max77658_fg_t *ctx, uint8_t reg_addr)
{
    uint16_t read_data;
    int status;

    status = max77658_fg_read_reg(ctx, reg_addr, &read_data);
    if (status != F_SUCCESS_0)
        return status;
    else
        return (int16_t)read_data;
}

/**
 * @brief        Write and Verify a MAX17055 register
 */
int max77658_fg_write_and_verify_reg(max77658_fg_t *ctx, uint8_t reg_addr, uint16_t write_data)
{
   int retries = 0;
   int read_status;
   int write_status;
   uint16_t read_data;

   do
   {
      write_status = max77658_fg_write_reg(ctx, reg_addr, write_data);
      if(write_status != F_SUCCESS_0)
      {
         return F_ERROR_1;
      }

      bsp_delay_ms(1);
      read_status = max77658_fg_read_reg(ctx, reg_addr, &read_data);
      if(read_status != F_SUCCESS_0)
      {
         return F_ERROR_1;
      }
   } while (write_data != read_data && retries++ < 3);

   if (write_data != read_data) return F_ERROR_1;
   return F_SUCCESS_0;
}

/**
 * @brief       Initialization Function for MAX17055.
 */
int max77658_fg_init(max77658_fg_t *ctx)
{
   int ret;
   int time_out = 1000; // Increase to 1 second safety margin
   int32_t status;
   uint16_t hibcfg_value;
   uint16_t version;

   // NOTE: ctx->pdata must be populated by the application before calling init
   
   LOG_INF("max77658_fg_init() Read Address = %X", ctx->device_address);

   status = max77658_fg_read_reg(ctx, VERSION_REG, &version);
   if (status != F_SUCCESS_0)
      return status;

   LOG_INF("max77658_fg_init() version: %d", version);
    
   ///STEP 0. Check for POR 
   int por_status = max77658_fg_check_POR_func(ctx);
   
   if (por_status == F_ERROR_5 && !CONFIG_FG_FORCE_EZ_CONFIG) {
       LOG_INF("No POR detected - skipping EZ config (preserving learned params)");
       return F_SUCCESS_0;
   }

   if (por_status == F_ERROR_5 && CONFIG_FG_FORCE_EZ_CONFIG) {
       LOG_WRN("No POR, but FORCE_EZ_CONFIG enabled -> reprogramming FG registers");
   } else {
       LOG_INF("POR detected - performing full EZ config initialization");
   }

   ///STEP 1. Check if FStat.DNR == 0 (Do not continue until FSTAT.DNR == 0)
   ret = max77658_fg_poll_flag_clear(ctx, FSTAT_REG, MAX17055_FSTAT_DNR, time_out);
   if (ret < F_SUCCESS_0) {
      LOG_ERR("FStat.DNR timeout");
      return ret;
   }

   ///STEP 1.2. Force exit from hibernate
   hibcfg_value = max77658_fg_forcedExitHiberMode(ctx);
   LOG_INF("max77658_fg_init() hibcfg_value: %d", hibcfg_value);

   ///STEP 2. Initialize configuration
   ///STEP 2.1. Load EZ Config
   max77658_fg_config_option_1(ctx);

   ///STEP 2.2. Poll ModelCFG.ModelRefresh bit for clear
   // Model refresh can take up to 500ms
   ret = max77658_fg_poll_flag_clear(ctx, MODELCFG_REG, MAX17055_MODELCFG_REFRESH, time_out);
   if(ret < F_SUCCESS_0) {
      LOG_ERR("ModelCFG.Refresh timeout");
      return ret;
   }
   ///STEP3. Restore original HibCfg
   max77658_fg_write_reg(ctx, HIBCFG_REG, hibcfg_value);

   /* Clear Status.POR */
   ret = max77658_fg_clear_POR_bit(ctx);
   if (ret < F_SUCCESS_0)
      return ret; 

   return F_SUCCESS_0;
}

/**
 * @brief      Check POR function
 */
int max77658_fg_check_POR_func(max77658_fg_t *ctx)
{
    uint16_t read_data;

    max77658_fg_read_reg(ctx, STATUS_REG, &read_data);
    if (!(read_data & MAX17055_STATUS_POR ) ) {
        return F_ERROR_5;  //POR not detected.
    } else
        return F_SUCCESS_0;
}

/**
 * @brief        clear POR bit function
 */
int max77658_fg_clear_POR_bit(max77658_fg_t *ctx)
{
    int status;
    uint16_t read_data;

    status = max77658_fg_read_reg(ctx, STATUS_REG, &read_data);
    if (status != F_SUCCESS_0)
        return F_ERROR_2;  //Device is not present in the i2c Bus
        
    // Clear POR bit (Bit 1)
    status = max77658_fg_write_and_verify_reg(ctx, STATUS_REG, (read_data & 0xFFFD));
    if (status != F_SUCCESS_0)
        return F_ERROR_1; //read or write error
    else
        return F_SUCCESS_0;
}

/**
 * @brief      Poll Flag clear Function.
 */
int max77658_fg_poll_flag_clear(max77658_fg_t *ctx, uint8_t reg_addr, int mask, int timeout)
{
    uint16_t data;
    int ret;

    // Convert timeout (ms) to loop count (assuming 10ms delay)
    int loops = timeout / 10; 
    if (loops < 1) loops = 1;

    do {
        bsp_delay_ms(10); // Check every 10ms
        ret = max77658_fg_read_reg(ctx, reg_addr, &data);

        if(ret < F_SUCCESS_0)
            return F_ERROR_1;

        if(!(data & mask))
            return F_SUCCESS_0;

        loops--;
    } while(loops > 0);

    return F_ERROR_4;
}

/**
 * @brief        Get Temperature Function from the MAX17055 TEMP register.
 */
int max77658_fg_get_temperature(max77658_fg_t *ctx)
{
    int ret;
    uint16_t temp;

    ret = max77658_fg_read_reg(ctx, TEMP_REG, &temp);
    if (ret < F_SUCCESS_0)
        return ret;

    /* The value is signed. */
    if (temp & 0x8000)
        temp |= 0xFFFF0000;

    /* The value is converted into centigrade scale */
    /* Units of LSB = 1 / 256 degree Celsius */
    temp >>= 8;

    return (int)temp;
}

/**
 * @brief        Forced Exit Hibernate Mode Function for MAX17055
 */
uint16_t max77658_fg_forcedExitHiberMode(max77658_fg_t *ctx)
{
    uint16_t hibcfg;

    /* Force exit from hibernate */
    //STEP 0: Store original HibCFG value
    max77658_fg_read_reg(ctx, HIBCFG_REG, &hibcfg);

    //STEP 1: Write to Soft-Wakeup Command Register
    max77658_fg_write_reg(ctx, VFSOC0_QH0_LOCK_REG, 0x90); //Soft-Wakeup from hibernate

    //STEP 2: Write to Hibernate Configuration register
    max77658_fg_write_reg(ctx, HIBCFG_REG, 0x0); //disable hibernate mode

    //STEP 3:Write to Soft-Wakeup Command Register
    max77658_fg_write_reg(ctx, VFSOC0_QH0_LOCK_REG, 0x0); //Clear All commands

    return hibcfg;
}

/**
 * @brief        EZ Config function
 */
uint16_t max77658_fg_config_option_1(max77658_fg_t *ctx)
{
   LOG_INF("max77658_fg_config_option_1() Read Address = %X", ctx->device_address);

    //STEP 2.1.1 EZ config values suggested by manufacturer.
    const int charger_th = 4275;
    const int chg_V_high = 51200; // scaling factor high voltage charger
    const int chg_V_low = 44138;
    const int param_EZ_FG1 = 0x8400; // Sets config bit for the charge voltage for the m5
    const int param_EZ_FG2 = 0x8000;
    uint16_t dpacc, ret;
    const int DIV_32 = 5;//DesignCap divide by 32 for EZ config

    //STEP 2.1.2 Store the EZ Config values into the appropriate registers.
    // FIXED: Use ctx->pdata instead of global pdata
    ret = max77658_fg_write_reg(ctx, DESIGNCAP_REG, ctx->pdata.designcap);
    ret = max77658_fg_write_reg(ctx, DQACC_REG, ctx->pdata.designcap >> DIV_32);
    ret = max77658_fg_write_reg(ctx, ICHGTERM_REG, ctx->pdata.ichgterm);
    ret = max77658_fg_write_reg(ctx, VEMPTY_REG, ctx->pdata.vempty);

    if (ctx->pdata.vcharge > charger_th) {
        dpacc = (ctx->pdata.designcap >> DIV_32) * chg_V_high / ctx->pdata.designcap;
        ret = max77658_fg_write_reg(ctx, DPACC_REG, dpacc);
        ret = max77658_fg_write_reg(ctx, MODELCFG_REG, param_EZ_FG1); 
    } else {
        dpacc = (ctx->pdata.designcap >> DIV_32) * chg_V_low / ctx->pdata.designcap;
        ret = max77658_fg_write_reg(ctx, DPACC_REG, dpacc);
        ret = max77658_fg_write_reg(ctx, MODELCFG_REG, param_EZ_FG2);
    }
    return ret;
}

/**
 * @brief        Config function option 2
 */
int32_t max77658_fg_config_option_2(max77658_fg_t *ctx)
{
   int32_t ret;

   /* Step 2.2: Option 2 Custom Short INI without OCV Table */
   ret = max77658_fg_write_reg(ctx, DESIGNCAP_REG, ctx->pdata.designcap);
   ret = max77658_fg_write_reg(ctx, ICHGTERM_REG, ctx->pdata.ichgterm);
   ret = max77658_fg_write_reg(ctx, VEMPTY_REG, ctx->pdata.vempty);
   max77658_fg_write_and_verify_reg(ctx, LEARNCFG_REG, ctx->pdata.learncfg); /* Optional */
   max77658_fg_write_and_verify_reg(ctx, FULLSOCTHR_REG, ctx->pdata.fullsocthr); /* Optional */

   ret = max77658_fg_write_reg(ctx, MODELCFG_REG, ctx->pdata.modelcfg);

   /* Poll ModelCFG.ModelRefresh bit for clear */
   ret = max77658_fg_poll_flag_clear(ctx, MODELCFG_REG, MAX17055_MODELCFG_REFRESH, 500);
   if(ret < 0)
   {
      LOG_ERR("Option2 model refresh not completed!");
      return ret;
   }

   ret = max77658_fg_write_reg(ctx, RCOMP0_REG, ctx->pdata.rcomp0);
   ret = max77658_fg_write_reg(ctx, TEMPCO_REG, ctx->pdata.tempco);
   ret = max77658_fg_write_reg(ctx, QRTABLE00_REG, ctx->pdata.qrtable00);
   ret = max77658_fg_write_reg(ctx, QRTABLE10_REG, ctx->pdata.qrtable10);
   ret = max77658_fg_write_reg(ctx, QRTABLE20_REG, ctx->pdata.qrtable20);  /* Optional */
   ret = max77658_fg_write_reg(ctx, QRTABLE30_REG, ctx->pdata.qrtable30);  /* Optional */

   return ret;
}

/**
 * @brief        Config function option 3
 */
int32_t max77658_fg_config_option_3(max77658_fg_t *ctx)
{
   return 0; // Not implemented
}

/**
 * @brief        Get reported Battery Capacity Function from MAX17055 Fuel Gauge
 */

int max77658_fg_get_battCAP(max77658_fg_t *ctx)
{
    int ret, design_rsense;
    uint16_t repcap_data;

    ret = max77658_fg_read_reg(ctx, REPCAP_REG, &repcap_data);
    if (ret < F_SUCCESS_0)
        return ret;
    else
        design_rsense = ctx->pdata.rsense;
    ret = max77658_fg_raw_cap_to_uAh((uint32_t)repcap_data, design_rsense);
    return ret;
}

/**
 * @brief        Get reported State Of Charge(SOC) Function from MAX17055 Fuel Gauge.
 */
int max77658_fg_get_SOC(max77658_fg_t *ctx)
{

    int ret;
    uint16_t soc_data;

    ret = max77658_fg_read_reg(ctx, REPSOC_REG, &soc_data);
    if (ret < F_SUCCESS_0)
        return ret;

    soc_data = soc_data >> 8; /* RepSOC LSB: 1/256 % */

    return soc_data;
}

/**
 * @brief       Get at rate Average State Of Charge(SOC) Function from MAX17055 Fuel Gauge.
 */
int max77658_fg_get_atAvSOC(max77658_fg_t *ctx)
{
    int ret;
    uint16_t atAvSOC_data;

    ret = max77658_fg_read_reg(ctx, AVSOC_REG, &atAvSOC_data);
    if (ret < F_SUCCESS_0)
        return ret; //Check errors if data is not correct

    atAvSOC_data = atAvSOC_data >> 8; /* avSOC LSB: 1/256 % */

    return atAvSOC_data;
}

/**
 * @brief        Get mix State Of Charge(SOC) Function for MAX17055 Fuel Gauge.
 */
int max77658_fg_get_mixSOC(max77658_fg_t *ctx)
{
    int ret;
    uint16_t mixSOC_data;

    ret = max77658_fg_read_reg(ctx, MIXSOC_REG, &mixSOC_data);
    if (ret < F_SUCCESS_0)
        return ret;

    mixSOC_data = mixSOC_data >> 8; /* RepSOC LSB: 1/256 % */

    return mixSOC_data;
}

/**
 * @brief       Get the Time to Empty(TTE) Function form MAX17055 Fuel Gauge.
 */
float max77658_fg_get_TTE(max77658_fg_t *ctx)
{

    int ret;
    uint16_t tte_data;
    float f_tte_data;

    ret = max77658_fg_read_reg(ctx, TTE_REG, &tte_data);
    if (ret < F_SUCCESS_0)
        return (float)ret;
    else
        f_tte_data = ((float)tte_data * 5.625f); /* TTE LSB: 5.625 sec */

    return f_tte_data;
}

/**
 * @brief       Get the at Time to Empty(atTTE) value Function for MAX17055 Fuel Gauge.
 */
float max77658_fg_get_atTTE(max77658_fg_t *ctx)
{

    int ret;
    uint16_t atTTE_data;
    float f_atTTE_data;

    ret = max77658_fg_read_reg(ctx, ATTTE_REG, &atTTE_data);
    if (ret < F_SUCCESS_0)
        return (float)ret; //Check for errors
    else
        f_atTTE_data = ((float)atTTE_data * 5.625f); /* TTE LSB: 5.625 sec */

    return  f_atTTE_data;
}

/**
 * @brief      Get the Time to Full(TTE) values Function for MAX17055 Fuel Gauge.
 */
float max77658_fg_get_TTF(max77658_fg_t *ctx)
{

    int ret;
    uint16_t ttf_data;
    float f_ttf_data;

    ret = max77658_fg_read_reg(ctx, TTF_REG, &ttf_data);
    if (ret < F_SUCCESS_0)
        return (float)ret;
    else
        f_ttf_data = ((float)ttf_data * 5.625f); /* TTE LSB: 5.625 sec */

    return  f_ttf_data;
}

/**
 * @brief       Get voltage of the cell Function for MAX17055 Fuel Gauge.
 */
int max77658_fg_get_Vcell(max77658_fg_t *ctx)
{
   int ret;
   uint16_t vcell_data;

   ret = max77658_fg_read_reg(ctx, VCELL_REG, &vcell_data);
   if (ret < F_SUCCESS_0)
   {
      return ret;
   }
   else
   {
      ret = max77658_fg_lsb_to_uvolts(vcell_data);
   }
   return ret;
}

/**
 * @brief       Gets Average voltage of the cell Function for MAX17055 Fuel Gauge.
 */
double max77658_fg_get_avgVcell(max77658_fg_t *ctx)
{

    int ret;
    uint16_t avgVcell_data;

    ret = max77658_fg_read_reg(ctx, AVGVCELL_REG, &avgVcell_data);
    if (ret < F_SUCCESS_0)
        return (double)ret;
    else
        return (double)max77658_fg_lsb_to_uvolts(avgVcell_data);
}

/**
 * @brief       Get current Function for MAX17055 Fuel Gauge.
 */
float max77658_fg_get_Current(max77658_fg_t *ctx)
{

    int ret,design_rsense;
    uint16_t curr_data;
    float f_ret;

    ret = max77658_fg_read_reg(ctx, CURRENT_REG, &curr_data);
    if (ret < F_SUCCESS_0)
        return (float)ret;
    else
        design_rsense = ctx->pdata.rsense;
    f_ret = max77658_fg_raw_current_to_uamps((uint32_t)curr_data, design_rsense);
    return f_ret;
}

/**
 * @brief       Get average current Function for MAX17055 Fuel Gauge.
 */
float max77658_fg_get_AvgCurrent(max77658_fg_t *ctx)
{
    int ret, design_rsense;
    uint16_t data;
    float avgCurr_data;

    ret = max77658_fg_read_reg(ctx, AVGCURRENT_REG, &data);
    if (ret < F_SUCCESS_0)
        return (float)ret;
    
    design_rsense = ctx->pdata.rsense;
    avgCurr_data = max77658_fg_raw_current_to_uamps((uint32_t)data, design_rsense);
    return avgCurr_data;
}

/**
 * @brief        lsb_to_uvolts Conversion Function
 */
int max77658_fg_lsb_to_uvolts(uint16_t lsb)
{
    int conv_2_uvolts;
    conv_2_uvolts = (lsb * 625) / 8; /* 78.125uV per bit */
    return conv_2_uvolts;
}

/**
 * @brief        raw_current_to_uamps Conversion Function
 * @param curr Raw 16-bit current register value (signed)
 * @param rsense_mohm Sense resistor value in milliohms
 * @return Current in microamps (µA)
 * 
 * Current register is signed 2's complement.
 * LSB = 1.5625µV / Rsense
 * For 50mΩ: LSB = 31.25µA per bit
 * Formula: Current(µA) = raw × (1562.5 / Rsense_mΩ)
 */
float max77658_fg_raw_current_to_uamps(uint32_t curr, int rsense_mohm)
{
    int16_t raw = (int16_t)curr; /* Treat as signed 16-bit */
    if (rsense_mohm <= 0) rsense_mohm = 50; /* Safety default */

    /* Current(µA) = raw × (1562.5 / Rsense_mΩ) */
    return ((float)raw * 1562.5f) / (float)rsense_mohm;
}

/**
 * @brief        raw_cap_to_uAh Conversion Function
 * @param raw_cap Raw capacity register value
 * @param rsense_mohm Sense resistor value in milliohms
 * @return Capacity in microamp-hours (µAh)
 * 
 * Capacity LSB = 5.0µVh / Rsense
 * For 50mΩ: LSB = 100µAh per bit
 * Formula: Capacity(µAh) = raw × (5000 / Rsense_mΩ)
 */
int max77658_fg_raw_cap_to_uAh(uint32_t raw_cap, int rsense_mohm)
{
    if (rsense_mohm <= 0) rsense_mohm = 50; /* Safety default */

    /* Capacity(µAh) = raw × (5000 / Rsense_mΩ) */
    float uAh = ((float)raw_cap * 5000.0f) / (float)rsense_mohm;
    return (int)(uAh + 0.5f); /* Round to nearest integer */
}

/**
 * @brief        Save Learned Parameters Function for battery Fuel Gauge model.
 * @param[in]   FG_params Pointer to Fuel Gauge Parameters struct
 */
int max77658_fg_save_Params(max77658_fg_t *ctx, saved_FG_params_t *FG_params)
{
    int ret;
    uint16_t data[5], value;
    ///STEP 1. Checks if the cycle register bit 2 has changed.
    ret = max77658_fg_read_reg(ctx, CYCLES_REG, &data[3]);
    value = data[3];
    if (ret < F_SUCCESS_0)
        return ret;
    
    //Check if the stored cycles value is different from the read Cycles_reg value
    else if (FG_params->cycles == value)
        return ret; // Exits without saving
    else {
        // NOTE: This bit toggle check logic is specific to Maxim's guide, keeping as is
        uint16_t check = FG_params->cycles ^ value;
        
        // Use the defined mask to check if significant bits changed (often bit 2 changes every 64%)
        // The original code used POR mask here which seems odd, but sticking to general logic:
        if ((check & 0x0004) == 0 && (value > FG_params->cycles)) 
        {
           // Optimization: Only save if significant change or forced
        }

        ///STEP 2. Save the capacity parameters for the specific battery.
        ret = max77658_fg_read_reg(ctx, RCOMP0_REG, &data[0]);
        if (ret < F_SUCCESS_0) return ret;
        FG_params->rcomp0 = data[0];

        ret = max77658_fg_read_reg(ctx, TEMPCO_REG, &data[1]);
        if (ret < F_SUCCESS_0) return ret;
        FG_params->temp_co = data[1];

        ret = max77658_fg_read_reg(ctx, FULLCAPREP_REG, &data[2]);
        if (ret < F_SUCCESS_0) return ret;
        FG_params->full_cap_rep = data[2];

        FG_params->cycles = data[3]; // Already read above

        ret = max77658_fg_read_reg(ctx, FULLCAPNOM_REG, &data[4]);
        if (ret < F_SUCCESS_0) return ret;
        FG_params->full_cap_nom = data[4];
        
        return ret;
    }
}

/**
 * @brief        Restore Parameters Function for battery Fuel Gauge model.
 * @param[in]    FG_params Pointer to Fuel Gauge Parameters struct
 */
int max77658_fg_restore_Params(max77658_fg_t *ctx, saved_FG_params_t *FG_params)
{
    int ret;
    uint16_t temp_data, fullcapnom_data, mixCap_calc, dQacc_calc;
    uint16_t dPacc_value = 0x0C80;//Set it to 200%

    ///STEP 1. Restoring capacity parameters
    // Fixed: Accessed via pointer ->
    max77658_fg_write_and_verify_reg(ctx, RCOMP0_REG, FG_params->rcomp0);
    max77658_fg_write_and_verify_reg(ctx, TEMPCO_REG, FG_params->temp_co);
    max77658_fg_write_and_verify_reg(ctx, FULLCAPNOM_REG, FG_params->full_cap_nom);

    bsp_delay_ms(350);

    ///STEP 2. Restore FullCap
    ret = max77658_fg_read_reg(ctx, FULLCAPNOM_REG, &fullcapnom_data);
    if (ret < F_SUCCESS_0)
        return ret;

    ret = max77658_fg_read_reg(ctx, MIXSOC_REG, &temp_data);
    if (ret < F_SUCCESS_0)
        return ret;

    mixCap_calc = (temp_data * fullcapnom_data) / 25600;

    max77658_fg_write_and_verify_reg(ctx, MIXCAP_REG, mixCap_calc);
    max77658_fg_write_and_verify_reg(ctx, FULLCAPREP_REG, FG_params->full_cap_rep);

    ///STEP 3. Write DQACC to 200% of Capacity and DPACC to 200%
    dQacc_calc = (FG_params->full_cap_nom / 16) ;

    max77658_fg_write_and_verify_reg(ctx, DPACC_REG, dPacc_value);
    max77658_fg_write_and_verify_reg(ctx, DQACC_REG, dQacc_calc);

    bsp_delay_ms(350);

    ///STEP 4. Restore Cycles register
    ret = max77658_fg_write_and_verify_reg(ctx, CYCLES_REG, FG_params->cycles);
    if (ret < F_SUCCESS_0)
        return ret;
    return ret;
}

/**
 * @brief        Function to Save Average Current to At Rate register.
 */
int max77658_fg_avCurr_2_atRate(max77658_fg_t *ctx)
{
    int ret;
    uint16_t avCurr_data;

    ret = max77658_fg_read_reg(ctx, AVGCURRENT_REG, &avCurr_data);
    if (ret < F_SUCCESS_0) {
        return F_ERROR_1;
    }

    //Write avCurrent to atRate Register
    ret = max77658_fg_write_reg(ctx, ATRATE_REG, avCurr_data);
    if (ret < F_SUCCESS_0) {
        return ret;
    }
    return F_SUCCESS_0;
}