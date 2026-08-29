/*
 * sim_core_decl.h
 *
 * Hand-written counterpart of simavr's generated core table - see
 * sim_core_config.h for why.  sim_avr.c defines AVR_KIND_DECL before
 * including this to instantiate the table itself.
 */
#ifndef __SIM_CORE_DECL_H__
#define __SIM_CORE_DECL_H__

#include "sim_core_config.h"

#if CONFIG_TINY85
extern avr_kind_t tiny85;
#endif

extern avr_kind_t * avr_kind[];

#ifdef AVR_KIND_DECL
avr_kind_t * avr_kind[] = {
#if CONFIG_TINY85
	&tiny85,
#endif
	NULL
};
#endif

#endif
