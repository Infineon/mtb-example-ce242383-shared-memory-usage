/******************************************************************************
* File Name:   main.c
*
* Description: This is the source code for PPCA CPU1 in the shared memory
*              example for ModusToolbox. PPCA CPU1 captures current:
*              it captures ADC conversions triggered by the EPU (on potentiometer
*              AIN10P / R262) and publishes the latest reading to shared memory
*              so the main CM33 core can consume it.
*
*              Design pattern used:
*              - ISR captures raw ADC value into a core-local variable
*                (no mutex, no blocking - ISR exits instantly).
*              - Main loop checks a data-ready flag and transfers the
*                core-local value to shared memory under mutex protection
*                using a non-blocking try-once lock.
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
#include "cycfg.h"
#include <stdbool.h>
#include <stdio.h>
#include "sm_vars.h"

/*******************************************************************************
* Macros
********************************************************************************/

/*******************************************************************************
* Global Variables
********************************************************************************/

/* Core-local ADC capture variable.
 * Written only by adc_isr() on this core - no shared memory, no mutex needed. */
static volatile uint32_t local_current_c1 = 0u;

/* Flag set by adc_isr() when fresh data is available.
 * Cleared by the main loop after the value is transferred to shared memory. */
static volatile bool current_data_ready = false;

/* ADC interrupt configuration structure */
cy_stc_sysint_t adc_intr_config =
{
    .intrSrc = ppca_epu_1_IRQn,
    .intrPriority = 1U,
};

/*******************************************************************************
* Function Prototypes
********************************************************************************/

/* ADC interrupt handler */
void adc_isr(void);

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
* This is the main function for PPCA CPU1. It does...
*    1. Registering and enabling the ADC EPU interrupt (adc_isr).
*    2. Routing the EPU IRQ1 signal to this PPCA core so it fires
*       when ADC3 conversion completes.
*    3. Running a background loop that transfers fresh ADC samples
*       to shared memory under mutex protection, but only when
*       adc_isr() has signalled that new data is available.
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
    
    /* Register adc_isr as the handler for EPU IRQ1 and enable it in the NVIC. */
    Cy_SysInt_Init(&adc_intr_config, &adc_isr);
    NVIC_EnableIRQ(adc_intr_config.intrSrc);
    
    /* Configure the EPU interrupt to the PPCA CPU0 on conversion completion of the ADC */
    Cy_PPCA_EPU_InterruptSourceSelect(ppca_0_epu_0_EPU_IRQ1_HW, false, epuIrqSrc1);
    Cy_PPCA_EPU_SetInterruptMask(ppca_0_epu_0_EPU_IRQ1_HW);

    /* Enable interrupts */
    __enable_irq();

    for(;;)
    {
        /* Gate on the data-ready flag to avoid writing the same value
         * repeatedly. The flag is set by adc_isr() on every new conversion. */
        if (current_data_ready)
        {
            /* Try to acquire the mutex in a non-blocking (try-once) manner.
             * If the main CM33 core currently holds mutex_c1, the lock fails
             * and this iteration is skipped. current_data_ready remains true
             * so the transfer is retried on the very next loop cycle, ensuring
             * no sample is dropped. */
            if (mutex_lock(&mutex_c1))
            {
                /* Critical section: write the latest current sample to shared
                 * memory. The main core reads sm_current_c1 while holding
                 * this same mutex, guaranteeing a consistent snapshot. */
                sm_current_c1 = local_current_c1;
                mutex_unlock(&mutex_c1);

                /* Clear the flag only after a successful transfer so that a
                 * missed lock does not silently discard the sample. */
                current_data_ready = false;
            }
        }
    }
}

/*******************************************************************************
* Function Name: adc_isr
********************************************************************************
* Summary:
*   EPU interrupt handler for PPCA CPU1. Fires when ADC3 completes a conversion
*   on channel 4 (potentiometer AIN10P / R262) to capture the current value.
*
*   Design: The ISR only captures the raw ADC value into a core-local variable
*   and sets a data-ready flag. It does NOT touch shared memory or attempt to
*   acquire a mutex. This keeps the ISR deterministic and non-blocking.
*   The mutex-protected transfer to shared memory is deferred to the main loop.
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void adc_isr()
{
    /* Clear the EPU interrupt flag before reading data to prevent
     * re-triggering the ISR while still inside the handler. */
    Cy_PPCA_EPU_ClearInterrupt(ppca_0_epu_0_EPU_IRQ1_HW);

    /* Capture ADC result into core-local variable only.
     * No mutex needed: only this ISR writes local_current_c1, so there
     * is no contention with any other context on this core.
     * The mutex-protected transfer to shared memory happens in the main loop. */
    local_current_c1 = Cy_PPCA_ADC_Read_ADC_Data(ADC3_HW, 4);

    /* Signal the main loop that a fresh sample is ready to be published
     * to shared memory. The flag is cleared by the main loop after a
     * successful mutex-protected transfer. */
    current_data_ready = true;
}
