/******************************************************************************
* File Name:   main.c
*
* Description: This is the source code for the secure main CM33 core in the
*              shared memory example for ModusToolbox. This core acts as the
*              supervisor: it initializes all peripherals, boots the two PPCA
*              cores, then continuously reads voltage (from PPCA CPU0) and
*              current (from PPCA CPU1) out of shared memory, computes power
*              (P = V x I), and prints all three values over UART.
*
*              Shared memory access is protected by two independent mutexes
*              (mutex_c0 for voltage, mutex_c1 for current). This core acquires
*              both sequentially before reading, ensuring a consistent snapshot
*              of both values for the power computation.
*
* Related Document: See README.md
*
*
********************************************************************************
* (c) 2026, Infineon Technologies AG, or an affiliate of Infineon
* Technologies AG. All rights reserved.
* This software, associated documentation and materials ("Software") is
* owned by Infineon Technologies AG or one of its affiliates ("Infineon")
* and is protected by and subject to worldwide patent protection, worldwide
* copyright laws, and international treaty provisions. Therefore, you may use
* this Software only as provided in the license agreement accompanying the
* software package from which you obtained this Software. If no license
* agreement applies, then any use, reproduction, modification, translation, or
* compilation of this Software is prohibited without the express written
* permission of Infineon.
*
* Disclaimer: UNLESS OTHERWISE EXPRESSLY AGREED WITH INFINEON, THIS SOFTWARE
* IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* INCLUDING, BUT NOT LIMITED TO, ALL WARRANTIES OF NON-INFRINGEMENT OF
* THIRD-PARTY RIGHTS AND IMPLIED WARRANTIES SUCH AS WARRANTIES OF FITNESS FOR A
* SPECIFIC USE/PURPOSE OR MERCHANTABILITY.
* Infineon reserves the right to make changes to the Software without notice.
* You are responsible for properly designing, programming, and testing the
* functionality and safety of your intended application of the Software, as
* well as complying with any legal requirements related to its use. Infineon
* does not guarantee that the Software will be free from intrusion, data theft
* or loss, or other breaches ("Security Breaches"), and Infineon shall have
* no liability arising out of any Security Breaches. Unless otherwise
* explicitly approved by Infineon, the Software may not be used in any
* application where a failure of the Product or any consequences of the use
* thereof can reasonably be expected to result in personal injury.
*******************************************************************************/

/*******************************************************************************
* Header Files
********************************************************************************/

#include "cy_pdl.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include "sm_vars.h"
#include "stdint.h"

/*******************************************************************************
* Macros
********************************************************************************/

/* These are the addresses where the core0 and core1 images are located. */
#define CORE0_IMAGE_ADDRESS   CYMEM_CM33_0_S_m33s_ppca0_nvm_C_S_START
#define CORE1_IMAGE_ADDRESS   CYMEM_CM33_0_S_m33s_ppca1_nvm_C_S_START

#define PPCA0_IMAGE_SIZE      CYMEM_CM33_0_S_ppca0_code_SIZE
#define PPCA1_IMAGE_SIZE      CYMEM_CM33_0_S_ppca1_code_SIZE

/*******************************************************************************
* Global Variables
********************************************************************************/
/* Debug UART variables */
static cy_stc_scb_uart_context_t    DEBUG_UART_context; /* DEBUG_UART context */
static mtb_hal_uart_t               DEBUG_UART_hal_obj; /* Debug DEBUG_UART HAL object */

/*******************************************************************************
* Function Prototypes
********************************************************************************/


/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
* This is the main function for the secure CM33 core. It does...
*    1. Initializing board peripherals and UART for debug output.
*    2. Configuring PPCA subsystem: PPCA block, EPU, AREF, ADC0, ADC3,
*       EPU processing units, combiners, PWMs, and sync trigger counter.
*    3. Enabling PPCA RAM and initializing the shared memory region.
*    4. Zeroing shared variables and setting mutexes to the unlocked state
*       before booting the PPCA cores to prevent stale data races.
*    5. Starting PPCA CPU0 (voltage capture) and CPU1 (current capture),
*       both running at the same sampling frequency.
*    6. Continuously reading voltage and current from shared memory under
*       mutex protection, computing power, and printing over UART every 200 ms.
*
*    Note: The following code example demonstrates the concept of shared memory
*          using middleware to emulate a power calculation.
*
* Parameters:
*  void
*
* Return:
*  int
*
*******************************************************************************/

int main(void)
{
    /* Result and status variables */
    cy_rslt_t result;
    cy_en_tcpwm_status_t status;

    /* Initialize the device and board peripherals */
    result = cybsp_init();
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Init DEBUG_UART */
    Cy_SCB_UART_Init(DEBUG_UART_HW, &DEBUG_UART_config, &DEBUG_UART_context);
    Cy_SCB_UART_Enable(DEBUG_UART_HW);

    /* Setup the HAL DEBUG_UART */
    result = mtb_hal_uart_setup(&DEBUG_UART_hal_obj, &DEBUG_UART_hal_config,
                                &DEBUG_UART_context, NULL);
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Initialize redirecting of low level IO */
    result = cy_retarget_io_init(&DEBUG_UART_hal_obj);
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Transmit header to the terminal */
    /* \x1b[2J\x1b[;H - ANSI ESC sequence for clear screen */
    printf("\x1b[2J\x1b[;H");
    printf("************************************************************\r\n");
    printf("PSOC Control C3M/P8: Shared memory example\r\n");
    printf("************************************************************\r\n\n");
    
    /* PPCA general settings.
     * Initializes and enables the PPCA configuration block, which controls
     * clocking and access permissions for the PPCA subsystem. */
    Cy_PPCA_CNFG_Init(ppca_0_ppca_cnfg_0_HW, &ppca_0_ppca_cnfg_0_config);
    Cy_PPCA_Enable(ppca_0_ppca_cnfg_0_HW);

    /* EPU general settings */
    Cy_PPCA_EPU_EnableExclusiveAccess(PPCA_EPU, true);
    Cy_PPCA_EPU_Enable(PPCA_EPU);
    
    /* Initialize and enable the analog reference (AREF).
     * AREF provides the stable voltage reference required by both ADCs. */
    Cy_PPCA_AREF_Init(AREF_HW, &AREF_config);
    Cy_PPCA_AREF_Enable(AREF_HW);

    /* Initialize and enable ADC0.
     * ADC0 channel 1 reads the voltage potentiometer (AIN0N / R221),
     * used by PPCA CPU0 to emulate the slow voltage loop. */
    Cy_PPCA_ADC_Init(ADC0_HW, &ADC0_config);
    Cy_PPCA_ADC_Enable(ADC0_HW);
    
    /* Initialize and enable ADC3.
     * ADC3 channel 4 reads the current potentiometer (AIN10P / R262),
     * used by PPCA CPU1 to emulate the fast current loop. */
    Cy_PPCA_ADC_Init(ADC3_HW, &ADC3_config);
    Cy_PPCA_ADC_Enable(ADC3_HW);
    
    /* Configure and enable EPU processing units and combiners.
     * The EPU (Event Processing Unit) chains hardware events into interrupt
     * triggers without CPU involvement:
     *   - PU_SYNC_TRIG / SYNC_TRIG_C0 / SYNC_TRIG_C1 : distribute the
     *     TCPWM sync trigger to both PPCA cores simultaneously.
     *   - PU_ADC0_C0 / TRIG_ADC0_C0  : trigger ADC0 conversion for CPU0.
     *   - PU_ADC0_C0_EOC / ADC0_C0_EOC: generate end-of-conversion event
     *     that fires the EPU IRQ0 handled by PPCA CPU0.
     *   - PU_ADC0_C1 / TRIG_ADC0_C1  : trigger ADC0 conversion for CPU1.
     *   - PU_ADC0_C1_EOC / ADC0_C1_EOC: generate end-of-conversion event
     *     that fires the EPU IRQ1 handled by PPCA CPU1. */
     Cy_PPCA_EPU_PU_T1_Configure(PU_SYNC_TRIG_HW, PU_SYNC_TRIG_INDEX, &PU_SYNC_TRIG_put1_config);
     Cy_PPCA_EPU_PU_T1_Enable(PU_SYNC_TRIG_HW, PU_SYNC_TRIG_INDEX,CY_ENABLE_ASYNC_BYPASS );
     Cy_PPCA_EPU_Combo_Configure(SYNC_TRIG_C0_HW, SYNC_TRIG_C0_INDEX, &SYNC_TRIG_C0_combo_config);
     Cy_PPCA_EPU_Combo_Configure(SYNC_TRIG_C1_HW, SYNC_TRIG_C1_INDEX, &SYNC_TRIG_C1_combo_config);
     
     Cy_PPCA_EPU_PU_T1_Configure(PU_ADC0_C0_HW, PU_ADC0_C0_INDEX, &PU_ADC0_C0_put1_config);
     Cy_PPCA_EPU_PU_T1_Enable(PU_ADC0_C0_HW, PU_ADC0_C0_INDEX,CY_ENABLE_ASYNC_BYPASS);
     Cy_PPCA_EPU_Combo_Configure(TRIG_ADC0_C0_HW, TRIG_ADC0_C0_INDEX, &TRIG_ADC0_C0_combo_config);
     
     Cy_PPCA_EPU_PU_T1_Configure(PU_ADC0_C0_EOC_HW, PU_ADC0_C0_EOC_INDEX, &PU_ADC0_C0_EOC_put1_config);
     Cy_PPCA_EPU_PU_T1_Enable(PU_ADC0_C0_EOC_HW, PU_ADC0_C0_EOC_INDEX,CY_ENABLE_ASYNC_BYPASS);
     Cy_PPCA_EPU_Combo_Configure(ADC0_C0_EOC_HW, ADC0_C0_EOC_INDEX, &ADC0_C0_EOC_combo_config);
     
     Cy_PPCA_EPU_PU_T1_Configure(PU_ADC0_C1_HW, PU_ADC0_C1_INDEX, &PU_ADC0_C1_put1_config);
     Cy_PPCA_EPU_PU_T1_Enable(PU_ADC0_C1_HW, PU_ADC0_C1_INDEX,CY_ENABLE_ASYNC_BYPASS);
     Cy_PPCA_EPU_Combo_Configure(TRIG_ADC0_C1_HW, TRIG_ADC0_C1_INDEX, &TRIG_ADC0_C1_combo_config);
     
     Cy_PPCA_EPU_PU_T1_Configure(PU_ADC0_C1_EOC_HW, PU_ADC0_C1_EOC_INDEX, &PU_ADC0_C1_EOC_put1_config);
     Cy_PPCA_EPU_PU_T1_Enable(PU_ADC0_C1_EOC_HW, PU_ADC0_C1_EOC_INDEX,CY_ENABLE_ASYNC_BYPASS);
     Cy_PPCA_EPU_Combo_Configure(ADC0_C1_EOC_HW, ADC0_C1_EOC_INDEX, &ADC0_C1_EOC_combo_config);

    /* Initialize and enable PWM_C0. */
    status = Cy_TCPWM_PWM_Init(PWM_C0_HW, PWM_C0_NUM, &PWM_C0_config);
    if(CY_TCPWM_SUCCESS != status)
    {
        CY_ASSERT(0);
    }
    Cy_TCPWM_PWM_Enable(PWM_C0_HW, PWM_C0_NUM);

    /* Initialize and enable PWM_C1. */
    status = Cy_TCPWM_PWM_Init(PWM_C1_HW, PWM_C1_NUM, &PWM_C1_config);
    if(CY_TCPWM_SUCCESS != status)
    {
        CY_ASSERT(0);
    }
    Cy_TCPWM_PWM_Enable(PWM_C1_HW, PWM_C1_NUM);
    
    /* Initializing the Synchronous trigger counter */
    status = Cy_TCPWM_Counter_Init(SYNC_TRIG_HW,SYNC_TRIG_NUM,&SYNC_TRIG_config);
    
    /* Initialization failed */
    if(CY_TCPWM_SUCCESS != status)
    {
        CY_ASSERT(0);
    }
    
    /* Enable the initialized counter */
    Cy_TCPWM_Counter_Enable (SYNC_TRIG_HW,SYNC_TRIG_NUM);
    
    /* Software start to the sync trigger */
    Cy_TCPWM_TriggerStart_Single(SYNC_TRIG_HW,SYNC_TRIG_NUM);

    /*  Enables PPCA RAM. */
    Cy_System_PPCA_RAM_Enable();
    
    /* Initialization of shared memory */
    sm_init();
    
    /* Zero all shared variables before releasing the PPCA cores.
     * This prevents the main loop from computing power from stale
     * or uninitialized values on the very first iteration. */
    sm_current_c1 = 0;
    sm_voltage_c0 = 0;
    sm_power = 0;
    
    /* Setting the mutex to unlock state */
    mutex_c0 = MUTEX_UNLOCKED;
    mutex_c1 = MUTEX_UNLOCKED;

    /* Start up PPCA cores */
    Cy_System_Init_CPU0_CPU1((void*)CORE0_IMAGE_ADDRESS, PPCA0_IMAGE_SIZE, (void*)CORE1_IMAGE_ADDRESS, PPCA1_IMAGE_SIZE);

    /* Enable interrupts on the main CM33 core. */
    __enable_irq();

    for (;;)
    {
        /* Acquire mutex_c0 first (guards sm_voltage_c0 written by PPCA CPU0).
         * Sequential acquisition avoids the logical error of entering the
         * critical section with only one mutex held. PPCA cores hold each
         * mutex only for a single variable copy, so spin time is minimal. */
        while(mutex_lock(&mutex_c0) == false) {}

        /* Acquire mutex_c1 next (guards sm_current_c1 written by PPCA CPU1). */
        while(mutex_lock(&mutex_c1) == false) {}
        
        /* Both mutexes are now held: read a consistent snapshot of voltage
         * and current, then compute instantaneous power (P = V x I). */
        sm_power = sm_current_c1 * sm_voltage_c0;
        
        /* Release mutex_c0 so PPCA CPU0 can update sm_voltage_c0 again. */
        mutex_unlock(&mutex_c0);

        /* Release mutex_c1 so PPCA CPU1 can update sm_current_c1 again. */
        mutex_unlock(&mutex_c1);

                /* Print all three values over UART for monitoring. */
        printf("Main Core: sm_current_c1 = %u, sm_voltage_c0 = %u sm_power = %u\r\n", (unsigned int)sm_current_c1,\
                            (unsigned int)sm_voltage_c0, (unsigned int)sm_power);
        
        /* 200 ms delay between prints. PPCA cores run independently during
         * this time, continuously updating shared memory with fresh samples. */
        Cy_SysLib_Delay(200);
    }
}
