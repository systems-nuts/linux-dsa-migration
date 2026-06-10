/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM idxd

#if !defined(_IDXD_FOLIO_DMA_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _IDXD_FOLIO_DMA_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(idxd_folios_copy_dma_begin,

	TP_PROTO(unsigned int nr_folios),

	TP_ARGS(nr_folios),

	TP_STRUCT__entry(
		__field(unsigned int, nr_folios)
	),

	TP_fast_assign(
		__entry->nr_folios = nr_folios;
	),

	TP_printk("nr_folios=%u",
		  __entry->nr_folios)
);

TRACE_EVENT(idxd_folios_copy_dma_end,

	TP_PROTO(unsigned int nr_folios, int ret),

	TP_ARGS(nr_folios, ret),

	TP_STRUCT__entry(
		__field(unsigned int, nr_folios)
		__field(int, ret)
	),

	TP_fast_assign(
		__entry->nr_folios = nr_folios;
		__entry->ret = ret;
	),

	TP_printk("nr_folios=%u ret=%d",
		  __entry->nr_folios,
		  __entry->ret)
);

#endif /* _IDXD_FOLIO_DMA_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE folio_dma_trace

#include <trace/define_trace.h>
