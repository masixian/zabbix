/*
** Zabbix
** Copyright (C) 2001-2016 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "daemon.h"
#include "zbxself.h"
#include "zbxtasks.h"
#include "log.h"
#include "db.h"
#include "dbcache.h"

#include "../../zabbix_server/scripts/scripts.h"

#define ZBX_TASKMANAGER_TIMEOUT		5

extern unsigned char	process_type, program_type;
extern int		server_num, process_num;

/******************************************************************************
 *                                                                            *
 * Function: tm_execute_remote_command                                        *
 *                                                                            *
 * Purpose: execute remote command task                                       *
 *                                                                            *
 * Parameters: taskid - [IN] the task identifier                              *
 *             clock  - [IN] the task creation time                           *
 *             ttl    - [IN] the task expiration period in seconds            *
 *             now    - [IN] the current time                                 *
 *                                                                            *
 * Return value: SUCCEED - the remote command was executed                    *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	tm_execute_remote_command(zbx_uint64_t taskid, int clock, int ttl, int now)
{
	DB_ROW		row;
	DB_RESULT	result;
	zbx_uint64_t	parent_taskid, hostid;
	zbx_tm_task_t	*task;
	int		ret = FAIL;
	zbx_script_t	script;
	char		*info = NULL, error[MAX_STRING_LEN];
	DC_HOST		host;

	result = DBselect("select command_type,execute_on,port,authtype,username,password,publickey,privatekey,"
					"command,parent_taskid,hostid"
				" from task_remote_command"
				" where taskid=" ZBX_FS_UI64,
				taskid);

	if (NULL != (row = DBfetch(result)))
	{
		task = zbx_tm_task_create(0, ZBX_TM_TASK_REMOTE_COMMAND_RESULT, ZBX_TM_STATUS_NEW, 0, 0, 0);

		if (0 != ttl && clock + ttl < now)
		{
			task->data = zbx_tm_remote_command_result_create(parent_taskid, FAIL,
					"The remote command has been expired.");
			goto finish;
		}

		ZBX_STR2UINT64(hostid, row[10]);
		if (FAIL == DCget_host_by_hostid(&host, hostid))
		{
			task->data = zbx_tm_remote_command_result_create(parent_taskid, FAIL, "Unknown host.");
			goto finish;
		}

		zbx_script_init(&script);

		ZBX_STR2UCHAR(script.type, row[0]);
		ZBX_STR2UCHAR(script.execute_on, row[1]);
		script.port = (0 == atoi(row[2]) ? "" : row[2]);
		ZBX_STR2UCHAR(script.authtype, row[3]);
		script.username = row[4];
		script.password = row[5];
		script.publickey = row[6];
		script.privatekey = row[7];
		script.command = row[8];
		ZBX_STR2UINT64(parent_taskid, row[9]);

		if (SUCCEED != (ret = zbx_script_execute(&script, &host, &info, error, sizeof(error))))
			task->data = zbx_tm_remote_command_result_create(parent_taskid, ret, error);
		else
			task->data = zbx_tm_remote_command_result_create(parent_taskid, ret, info);

		zbx_free(info);

		DBbegin();

		zbx_tm_save_task(task);
		zbx_tm_task_free(task);
finish:;
	}
	else
		DBbegin();

	DBfree_result(result);

	DBexecute("delete from task where taskid=" ZBX_FS_UI64, taskid);

	DBcommit();

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: tm_process_tasks                                                 *
 *                                                                            *
 * Purpose: process task manager tasks depending on task type                 *
 *                                                                            *
 * Return value: The number of successfully processed tasks                   *
 *                                                                            *
 ******************************************************************************/
static int	tm_process_tasks(int now)
{
	DB_ROW		row;
	DB_RESULT	result;
	int		ret, processed_num = 0, clock, ttl;
	zbx_uint64_t	taskid;
	unsigned char	type;


	result = DBselect("select taskid,type,clock,ttl"
				" from task"
				" where status=%d"
					" and type=%d"
				" order by taskid",
			ZBX_TM_STATUS_NEW, ZBX_TM_TASK_REMOTE_COMMAND);

	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(taskid, row[0]);
		ZBX_STR2UCHAR(type, row[1]);
		clock = atoi(row[2]);
		ttl = atoi(row[3]);

		switch (type)
		{
			case ZBX_TM_TASK_REMOTE_COMMAND:
				ret = tm_execute_remote_command(taskid, clock, ttl, now);
				break;
			default:
				THIS_SHOULD_NEVER_HAPPEN;
				ret = FAIL;
				break;
		}

		if (FAIL != ret)
			processed_num++;
	}
	DBfree_result(result);

	return processed_num;
}

ZBX_THREAD_ENTRY(taskmanager_thread, args)
{
	double	sec1, sec2;
	int	tasks_num = 0, sleeptime, nextcheck;

	process_type = ((zbx_thread_args_t *)args)->process_type;
	server_num = ((zbx_thread_args_t *)args)->server_num;
	process_num = ((zbx_thread_args_t *)args)->process_num;

	zabbix_log(LOG_LEVEL_INFORMATION, "%s #%d started [%s #%d]", get_program_type_string(program_type),
			server_num, get_process_type_string(process_type), process_num);

	zbx_setproctitle("%s [connecting to the database]", get_process_type_string(process_type));
	DBconnect(ZBX_DB_CONNECT_NORMAL);

	sec1 = zbx_time();
	sec2 = sec1;

	sleeptime = ZBX_TASKMANAGER_TIMEOUT - (int)sec1 % ZBX_TASKMANAGER_TIMEOUT;

	zbx_setproctitle("%s [started, idle %d sec]", get_process_type_string(process_type), sleeptime);

	for (;;)
	{
		zbx_sleep_loop(sleeptime);

		zbx_handle_log();

		zbx_setproctitle("%s [processing tasks]", get_process_type_string(process_type));

		sec1 = zbx_time();
		tasks_num = tm_process_tasks((int)sec1);
		sec2 = zbx_time();

		nextcheck = (int)sec1 - (int)sec1 % ZBX_TASKMANAGER_TIMEOUT + ZBX_TASKMANAGER_TIMEOUT;

		if (0 > (sleeptime = nextcheck - (int)sec2))
			sleeptime = 0;

		zbx_setproctitle("%s [processed %d task(s) in " ZBX_FS_DBL " sec, idle %d sec]",
				get_process_type_string(process_type), tasks_num, sec2 - sec1, sleeptime);
	}
}
