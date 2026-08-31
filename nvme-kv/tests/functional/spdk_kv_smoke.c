/* SPDX-FileCopyrightText: Copyright (c) 2026 Cisco and/or its affiliates. */
/* SPDX-License-Identifier: Apache-2.0 */
/* Direct SPDK validation of the NVMe Key-Value command set. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <spdk/env.h>
#include <spdk/nvme.h>
#include <spdk/nvme_kv.h>

struct command_result {
	bool done;
	bool success;
};

struct device_state {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns *ns;
};

static void
complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct command_result *result = arg;

	result->success = !spdk_nvme_cpl_is_error(completion);
	result->done = true;
}

static int
wait_for_completion(struct spdk_nvme_qpair *qpair, struct command_result *result)
{
	int rc;

	while (!result->done) {
		rc = spdk_nvme_qpair_process_completions(qpair, 0);
		if (rc < 0) {
			return rc;
		}
	}
	return result->success ? 0 : -EIO;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct device_state *state = cb_ctx;

	(void)trid;
	(void)opts;
	return state->ctrlr == NULL;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct device_state *state = cb_ctx;
	uint32_t nsid;

	(void)trid;
	(void)opts;
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

		if (ns != NULL && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV &&
		    spdk_nvme_kv_ns_get_data(ns) != NULL) {
			state->ctrlr = ctrlr;
			state->ns = ns;
			return;
		}
	}

	spdk_nvme_detach(ctrlr);
}

static int
run_smoke(struct device_state *state)
{
	static const uint8_t key[] = "lmcache-kv-smoke";
	static const uint8_t value[] = "lmcache-spdk-nvme-kv-smoke";
	const struct spdk_nvme_kv_ns_data *ns_data;
	const struct spdk_nvme_kv_format *format;
	struct spdk_nvme_qpair *qpair = NULL;
	struct command_result result = {};
	uint8_t *write_buffer = NULL;
	uint8_t *read_buffer = NULL;
	uint8_t *list_buffer = NULL;
	uint32_t value_limit;
	int rc = 1;

	ns_data = spdk_nvme_kv_ns_get_data(state->ns);
	format = &ns_data->kvf[ns_data->kvfc.kvfi];
	value_limit = format->kvvml;
	if (format->kvkml < sizeof(key) - 1 || value_limit < sizeof(value) - 1) {
		fprintf(stderr, "KV namespace limits are too small: key=%u value=%u\n",
			format->kvkml, value_limit);
		return 1;
	}

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(state->ctrlr, NULL, 0);
	if (qpair == NULL) {
		fprintf(stderr, "allocating KV I/O qpair failed\n");
		goto out;
	}
	write_buffer = spdk_dma_zmalloc(sizeof(value) - 1, 4096, NULL);
	read_buffer = spdk_dma_zmalloc(sizeof(value) - 1, 4096, NULL);
	list_buffer = spdk_dma_zmalloc(value_limit, 4096, NULL);
	if (write_buffer == NULL || read_buffer == NULL || list_buffer == NULL) {
		fprintf(stderr, "allocating DMA buffers failed\n");
		goto out;
	}
	memcpy(write_buffer, value, sizeof(value) - 1);

	rc = spdk_nvme_kv_store(state->ns, qpair, key, sizeof(key) - 1, write_buffer,
				sizeof(value) - 1, complete, &result,
				SPDK_NVME_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS);
	if (rc != 0 || wait_for_completion(qpair, &result) != 0) {
		fprintf(stderr, "KV STORE failed\n");
		rc = 1;
		goto out;
	}

	result = (struct command_result){};
	rc = spdk_nvme_kv_exist(state->ns, qpair, key, sizeof(key) - 1, complete, &result);
	if (rc != 0 || wait_for_completion(qpair, &result) != 0) {
		fprintf(stderr, "KV EXIST after STORE failed\n");
		rc = 1;
		goto out;
	}

	result = (struct command_result){};
	rc = spdk_nvme_kv_retrieve(state->ns, qpair, key, sizeof(key) - 1, read_buffer,
				   sizeof(value) - 1, complete, &result, 0);
	if (rc != 0 || wait_for_completion(qpair, &result) != 0 ||
	    memcmp(read_buffer, value, sizeof(value) - 1) != 0) {
		fprintf(stderr, "KV RETRIEVE failed or returned the wrong value\n");
		rc = 1;
		goto out;
	}

	result = (struct command_result){};
	rc = spdk_nvme_kv_list(state->ns, qpair, NULL, 0, list_buffer, value_limit,
				complete, &result);
	if (rc != 0 || wait_for_completion(qpair, &result) != 0) {
		fprintf(stderr, "KV LIST failed\n");
		rc = 1;
		goto out;
	}

	result = (struct command_result){};
	rc = spdk_nvme_kv_delete(state->ns, qpair, key, sizeof(key) - 1, complete, &result);
	if (rc != 0 || wait_for_completion(qpair, &result) != 0) {
		fprintf(stderr, "KV DELETE failed\n");
		rc = 1;
		goto out;
	}

	result = (struct command_result){};
	rc = spdk_nvme_kv_exist(state->ns, qpair, key, sizeof(key) - 1, complete, &result);
	if (rc != 0 || wait_for_completion(qpair, &result) == 0) {
		fprintf(stderr, "KV EXIST unexpectedly succeeded after DELETE\n");
		rc = 1;
		goto out;
	}

	printf("SPDK NVMe-KV smoke test passed (max value: %u bytes)\n", value_limit);
	rc = 0;
out:
	spdk_dma_free(write_buffer);
	spdk_dma_free(read_buffer);
	spdk_dma_free(list_buffer);
	if (qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}
	return rc;
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts env_opts = {};
	struct spdk_nvme_transport_id trid = {};
	struct device_state state = {};
	int rc = 1;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <PCIe-BDF>\n", argv[0]);
		return 2;
	}

	env_opts.opts_size = sizeof(env_opts);
	spdk_env_opts_init(&env_opts);
	env_opts.name = "lmcache_spdk_kv_smoke";
	env_opts.mem_size = 64;
	if (spdk_env_init(&env_opts) < 0) {
		fprintf(stderr, "initializing the SPDK environment failed\n");
		return 1;
	}

	spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", argv[1]);
	if (spdk_nvme_probe(&trid, &state, probe_cb, attach_cb, NULL) != 0 ||
	    state.ctrlr == NULL || state.ns == NULL) {
		fprintf(stderr, "no Key-Value namespace attached at %s\n", argv[1]);
		goto out;
	}

	rc = run_smoke(&state);
out:
	if (state.ctrlr != NULL) {
		spdk_nvme_detach(state.ctrlr);
	}
	spdk_env_fini();
	return rc;
}
