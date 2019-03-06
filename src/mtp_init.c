/*
 * Copyright (c) 2012, 2013 Samsung Electronics Co., Ltd.
 * Copyright (c) 2019 Collabora Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <glib.h>
#include <glib-object.h>
#include <malloc.h>
#include "mtp_init.h"
#include "mtp_config.h"
#include "mtp_thread.h"
#include "mtp_support.h"
#include "mtp_device.h"
#include "mtp_event_handler.h"
#include "mtp_cmd_handler.h"
#include "mtp_inoti_handler.h"
#include "mtp_transport.h"
#include "mtp_util.h"
#include "mtp_media_info.h"
#include "mtp_usb_driver.h"

/*
 * GLOBAL AND EXTERN VARIABLES
 */
extern pthread_t g_eh_thrd;
extern pthread_mutex_t g_cmd_inoti_mutex;
extern mtp_bool g_is_sync_estab;

mtp_mgr_t g_mtp_mgr;
mtp_config_t g_conf;

/*
 * STATIC VARIABLES AND FUNCTIONS
 */
static GMainLoop *g_mainloop = NULL;
static mtp_mgr_t *g_mgr = &g_mtp_mgr;
static void __read_mtp_conf(void);
static void __init_mtp_info(void);
static void __mtp_exit(void);
/*
 * FUNCTIONS
 */

/*
 * static void __mtp_exit(void)
 * This function send MTP stopped state to event handler thread and MTP UI
 * @param[in]		None.
 * @param[out]		None.
 * @return		None.
 */
/* LCOV_EXCL_START */
static void __mtp_exit(void)
{
	DBG("## Terminate all threads");
	if (g_eh_thrd && g_eh_thrd != pthread_self()) {
		_eh_send_event_req_to_eh_thread(EVENT_USB_REMOVED, 0, 0, NULL);
		if (_util_thread_join(g_eh_thrd, NULL) == FALSE)
			ERR("_util_thread_join() Fail");

		g_eh_thrd = 0;
	}

	DBG("## Terminate main loop");

	g_main_loop_quit(g_mainloop);

	if (g_eh_thrd == pthread_self())
		_util_thread_exit("Event handler stopped itself");

	return;
}

/* LCOV_EXCL_STOP */

void _mtp_init(void)
{
	mtp_int32 error = 0;

	DBG("Initialization start!");

	__read_mtp_conf();

	if (g_conf.mmap_threshold) {
		if (!mallopt(M_MMAP_THRESHOLD, g_conf.mmap_threshold))
			ERR("mallopt(M_MMAP_THRESHOLD) Fail");

		if (!mallopt(M_TRIM_THRESHOLD, g_conf.mmap_threshold * 2))
			ERR("mallopt(M_TRIM_THRESHOLD) Fail");
	}

	__init_mtp_info();

	_transport_init_status_info();
	_transport_set_mtp_operation_state(MTP_STATE_INITIALIZING);

	if (g_mgr->ftemp_st.temp_buff == NULL) {
		/* Allocate memory for temporary */
		g_mgr->ftemp_st.temp_buff = (mtp_char *)g_malloc(g_conf.write_file_size);
		if (g_mgr->ftemp_st.temp_buff == NULL) {
			ERR("memory allocation fail");
			goto MTP_INIT_FAIL;
		}
	}

	/* External Storage */
	{
	/* LCOV_EXCL_START */
		char ext_path[MTP_MAX_PATHNAME_SIZE + 1] = { 0 };
		_util_get_external_path(ext_path);
		if (access(ext_path, F_OK) < 0) {
			if (FALSE == _util_dir_create((const mtp_char *)ext_path, &error)) {
				ERR("Cannot make directory!! [%s]\n",
						ext_path);
				goto MTP_INIT_FAIL;
			}
		}
	/* LCOV_EXCL_STOP */
	}

	/* Set mtpdeviceinfo */
	_init_mtp_device();

	_features_supported_info();

	/* Install storage */
	_device_install_storage();

#ifdef MTP_SUPPORT_OBJECTADDDELETE_EVENT
	_inoti_init_filesystem_evnts();
#endif /*MTP_SUPPORT_OBJECTADDDELETE_EVENT*/

	return;

MTP_INIT_FAIL:
	/* Set MTP state to stopped */
	_transport_set_mtp_operation_state(MTP_STATE_STOPPED);
	mtp_end_event();

	return;
}

void _mtp_deinit(void)
{
	_cmd_hdlr_reset_cmd(&g_mgr->hdlr);

	/* initialize MTP_USE_FILE_BUFFER*/
	if (g_mgr->ftemp_st.temp_buff != NULL) {
		g_free(g_mgr->ftemp_st.temp_buff);
		g_mgr->ftemp_st.temp_buff = NULL;
	}

#ifdef MTP_SUPPORT_OBJECTADDDELETE_EVENT
	_inoti_deinit_filesystem_events();
#endif /*MTP_SUPPORT_OBJECTADDDELETE_EVENT*/

	return;
}

static void __print_mtp_conf(void)
{
	if (g_conf.is_init == false) {
		ERR("g_conf is not initialized");
		return;
	}

	DBG("MMAP_THRESHOLD : %d\n", g_conf.mmap_threshold);
	DBG("INIT_RX_IPC_SIZE : %d\n", g_conf.init_rx_ipc_size);
	DBG("INIT_TX_IPC_SIZE : %d\n", g_conf.init_tx_ipc_size);
	DBG("MAX_RX_IPC_SIZE : %d\n", g_conf.max_rx_ipc_size);
	DBG("MAX_TX_IPC_SIZE : %d\n", g_conf.max_tx_ipc_size);
	DBG("READ_USB_SIZE : %d\n", g_conf.read_usb_size);
	DBG("WRITE_USB_SIZE : %d\n", g_conf.write_usb_size);
	DBG("READ_FILE_SIZE : %d\n", g_conf.read_file_size);
	DBG("WRITE_FILE_SIZE : %d\n", g_conf.write_file_size);
	DBG("MAX_IO_BUF_SIZE : %d\n\n", g_conf.max_io_buf_size);

	DBG("SUPPORT_PTHEAD_SHCED : %s\n", g_conf.support_pthread_sched ? "Support" : "Not support");
	DBG("INHERITSCHED : %c\n", g_conf.inheritsched);
	DBG("SCHEDPOLICY : %c\n", g_conf.schedpolicy);
	DBG("FILE_SCHEDPARAM: %d\n", g_conf.file_schedparam);
	DBG("USB_SCHEDPARAM: %d\n\n", g_conf.usb_schedparam);
}

static void __read_mtp_conf(void)
{
	FILE *fp;
	char buf[256];
	char *token;
	char *saveptr = NULL;

	g_conf.mmap_threshold = MTP_MMAP_THRESHOLD;

	g_conf.read_usb_size = MTP_READ_USB_SIZE;
	g_conf.write_usb_size = MTP_WRITE_USB_SIZE;

	g_conf.read_file_size = MTP_READ_FILE_SIZE;
	g_conf.write_file_size = MTP_WRITE_FILE_SIZE;

	g_conf.init_rx_ipc_size = MTP_INIT_RX_IPC_SIZE;
	g_conf.init_tx_ipc_size = MTP_INIT_TX_IPC_SIZE;

	g_conf.max_rx_ipc_size = MTP_MAX_RX_IPC_SIZE;
	g_conf.max_tx_ipc_size = MTP_MAX_TX_IPC_SIZE;

	g_conf.max_io_buf_size = MTP_MAX_IO_BUF_SIZE;
	g_conf.read_file_delay = MTP_READ_FILE_DELAY;

	if (MTP_SUPPORT_PTHREAD_SCHED) {
		g_conf.support_pthread_sched = MTP_SUPPORT_PTHREAD_SCHED;
		g_conf.inheritsched = MTP_INHERITSCHED;
		g_conf.schedpolicy = MTP_SCHEDPOLICY;
		g_conf.file_schedparam = MTP_FILE_SCHEDPARAM;
		g_conf.usb_schedparam = MTP_USB_SCHEDPARAM;
	}

	fp = fopen(MTP_CONFIG_FILE_PATH, "r");
	if (fp == NULL) {
		/* LCOV_EXCL_START */
		DBG("Default configuration is used");
		g_conf.is_init = true;

		__print_mtp_conf();
		/* LCOV_EXCL_STOP */
		return;
	}

	while (fgets(buf, sizeof(buf), fp)) {
		if (buf[0] == '#' || buf[0] == '\n')
			continue;

		token = strrchr(buf, '\n');
		if (token == NULL) {
			ERR("g_conf is too long");
			break;
		}
		*token = '\0';

		token = strtok_r(buf, "=", &saveptr);
		if (token == NULL)
			continue;	//	LCOV_EXCL_LINE

		if (strcasecmp(token, "mmap_threshold") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.mmap_threshold = atoi(token);

		} else if (strcasecmp(token, "init_rx_ipc_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.init_rx_ipc_size = atoi(token);

		} else if (strcasecmp(token, "init_tx_ipc_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.init_tx_ipc_size = atoi(token);

		} else if (strcasecmp(token, "max_rx_ipc_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.max_rx_ipc_size = atoi(token);

		} else if (strcasecmp(token, "max_tx_ipc_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.max_tx_ipc_size = atoi(token);

		} else if (strcasecmp(token, "read_usb_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.read_usb_size = atoi(token);

		} else if (strcasecmp(token, "write_usb_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.write_usb_size = atoi(token);

		} else if (strcasecmp(token, "read_file_size") == 0) {
			/* LCOV_EXCL_START */
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.read_file_size = atoi(token);

		} else if (strcasecmp(token, "write_file_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.write_file_size = atoi(token);
			/* LCOV_EXCL_STOP */
		} else if (strcasecmp(token, "max_io_buf_size") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.max_io_buf_size = atoi(token);

		} else if (strcasecmp(token, "read_file_delay") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.read_file_delay = atoi(token);

		} else if (strcasecmp(token, "support_pthread_sched") == 0) {
			/* LCOV_EXCL_START */
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.support_pthread_sched = atoi(token) ? true : false;

		} else if (strcasecmp(token, "inheritsched") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.inheritsched = *token;

		} else if (strcasecmp(token, "schedpolicy") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.schedpolicy = *token;

		} else if (strcasecmp(token, "file_schedparam") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.file_schedparam = atoi(token);

		} else if (strcasecmp(token, "usb_schedparam") == 0) {
			token = strtok_r(NULL, "=", &saveptr);
			if (token == NULL)
				continue;	//	LCOV_EXCL_LINE

			g_conf.usb_schedparam = atoi(token);
		/* LCOV_EXCL_STOP */
		} else {
			ERR("Unknown option : %s\n", buf);
		}
	}
	fclose(fp);
	g_conf.is_init = true;

	__print_mtp_conf();
	return;
}

void __init_mtp_info(void)
{
	/* initialize struct one time*/
	memset(&g_mgr->ftemp_st, 0, sizeof(g_mgr->ftemp_st));
	memset(&g_mgr->hdlr, 0, sizeof(g_mgr->hdlr));
	memset(&g_mgr->meta_info, 0, sizeof(g_mgr->meta_info));

	return ;
}

void _features_supported_info(void)
{
	DBG("***********************************************************");
	DBG("### MTP Information ###");
	DBG("### 1. Solution		: SLP");
	DBG("### 2. MTP Version		: 1.0");
	DBG("### 3. DB Limitation       : Reference(%d)\n", MTP_MAX_REFDB_ROWCNT);

	DBG("***********************************************************");
	DBG("### Extension ###");
	if (_get_oma_drm_status() == TRUE)
		DBG("### 2. OMADRM		: [ON]");
	else
		DBG("### 2. OMADRM		: [OFF]");

	DBG("***********************************************************");
	DBG("### Feature ###");

#ifdef MTP_SUPPORT_SET_PROTECTION
	DBG("### 3. MTP_SUPPORT_SET_PROTECTION	: [ON]");
#else /* MTP_SUPPORT_SET_PROTECTION */
	DBG("### 3. MTP_SUPPORT_SET_PROTECTION	: [OFF]");
#endif /* MTP_SUPPORT_SET_PROTECTION */
	DBG("***********************************************************");
	return;
}

/*
 * void mtp_end_event(void)
 * This function terminates mtp.
 * It must not be called in gthr_mtp_event thread.
 * It makes dead lock state if it is called in gthr_mtp_event thread.
 */
/* LCOV_EXCL_START */
void mtp_end_event(void)
{
	__mtp_exit();
}
/* LCOV_EXCL_STOP */

static inline int _main_init()
{
	pthread_mutexattr_t mutex_attr;

	pthread_mutexattr_init(&mutex_attr);
	pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
	if (0 != pthread_mutex_init(&g_cmd_inoti_mutex, &mutex_attr)) {
		ERR("pthread_mutex_init() Fail");
		_util_print_error();
		pthread_mutexattr_destroy(&mutex_attr);
		return MTP_ERROR_GENERAL;
	}
	pthread_mutexattr_destroy(&mutex_attr);

	if (_eh_handle_usb_events(USB_INSERTED) == FALSE) {
		ERR("_eh_handle_usb_events() Fail");
		return MTP_ERROR_GENERAL;
	}

	g_mainloop = g_main_loop_new(NULL, FALSE);
	if (g_mainloop == NULL) {
		ERR("g_mainloop is NULL");
		return MTP_ERROR_GENERAL;
	}

	return MTP_ERROR_NONE;
}

int main(int argc, char *argv[])
{
	mtp_int32 ret;

	if (_eh_register_notification_callbacks() == FALSE) {
		ERR("_eh_register_notification_callbacks() Fail");
		return MTP_ERROR_GENERAL;
	}

	ret = _main_init();
	if (MTP_ERROR_NONE != ret) {
		ERR("_main_init() Fail(%d)", ret);
		_util_media_content_disconnect();
		return MTP_ERROR_GENERAL;
	}
	DBG("MTP UID = [%u] and GID = [%u]\n", getuid(), getgid());

	g_main_loop_run(g_mainloop);

	DBG("######### MTP TERMINATED #########");

	return MTP_ERROR_NONE;
}
