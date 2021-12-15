/**
 * (C) Copyright 2016-2021 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef __DAOS_PIPE_RPC_H__
#define __DAOS_PIPE_RPC_H__

#include <daos/rpc.h>
#include <daos/pipeline.h>
#include <daos/object.h>
#include "pipeline_internal.h"

#define DAOS_PIPELINE_VERSION 1
/* LIST of internal RPCS in form of:
 * OPCODE, flags, FMT, handler, corpc_hdlr and name
 */
#define PIPELINE_PROTO_CLI_RPC_LIST					\
	X(DAOS_PIPELINE_RPC_RUN,					\
		0, &CQF_pipeline_run,					\
		ds_pipeline_run_handler, NULL, "pipeline_run")

#define X(a, b, c, d, e, f) a
enum pipeline_rpc_opc {
	PIPELINE_PROTO_CLI_RPC_LIST,
	PIPELINE_PROTO_CLI_COUNT,
	PIPELINE_PROTO_CLI_LAST = PIPELINE_PROTO_CLI_COUNT - 1,
};
#undef X

extern struct crt_proto_format pipeline_proto_fmt;

static inline char *
obj_opc_to_str(crt_opcode_t opc)
{
	switch (opc) {
#define X(a, b, c, d, e, f) case a: return f;
		PIPELINE_PROTO_CLI_RPC_LIST
#undef X
	}
	return "unknown";
}

#define DAOS_ISEQ_PIPELINE_RUN	/* input fields */			\
	((daos_pipeline_t)	(pri_pipe)		CRT_VAR)	\
	((daos_unit_oid_t)	(pri_oid)		CRT_VAR)	\
	((uuid_t)		(pri_pool_uuid)		CRT_VAR)	\
	((uuid_t)		(pri_co_hdl)		CRT_VAR)	\
	((uuid_t)		(pri_co_uuid)		CRT_VAR)	\
	((uint64_t)		(pri_epoch)		CRT_VAR)	\
	((uint64_t)		(pri_epoch_first)	CRT_VAR)	\
	((uint32_t)		(pri_target)		CRT_VAR)	\
	((uint32_t)		(pri_pad32_1)		CRT_VAR)

#define DAOS_OSEQ_PIPELINE_RUN	/*output fields */			\
	((int32_t)		(pro_ret)		CRT_VAR)	\
	((uint32_t)		(pro_pad32_1)		CRT_VAR)	\
	((uint64_t)		(pro_pong)		CRT_VAR)	\
	((uint64_t)		(pro_epoch)		CRT_VAR)


CRT_RPC_DECLARE(pipeline_run, DAOS_ISEQ_PIPELINE_RUN, DAOS_OSEQ_PIPELINE_RUN)


#endif /* __DAOS_PIPE_RPC_H__ */
