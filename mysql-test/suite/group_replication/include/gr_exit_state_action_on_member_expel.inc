###############################################################################
#
# group_replication_exit_state_action sysvar specifies which action is taken by
# a server once it has involuntarily left the group. Currently there are only
# two actions: either the server continues running but with super_read_only
# enabled (READ_ONLY) or it aborts (ABORT_SERVER).
#
# In order to verify the expected behaviour, this test shall test that the
# specified exit action is executed correctly after an expel by the other
# members of the group.
#
# Test:
# 0) Setup group of 3 members (M1, M2 and M3).
# 1) Force expel of member 1.
# 1.1) Verify that member 1 went to super_read_only mode.
# 2) Relaunch member 1 with exit state action to ABORT_SERVER.
# 3) Force expel again of member 1.
# 3.1) Verify that member 1 aborted.
# 4) Relaunch member 1.
# 5) Cleanup.
#
################################################################################
# Test involves sending SIGSTOP and SIGCONT signals using kill Linux command.
--source include/linux.inc
--source include/big_test.inc
--source include/have_group_replication_plugin.inc

--echo
--echo #########################################################################
--echo # 0) Setup group of 3 members (M1, M2 and M3).
--echo #########################################################################
--echo
--let $rpl_server_count= 3
--source include/group_replication.inc

--let $rpl_connection_name= server1
--source include/connection.inc
SET SESSION sql_log_bin = 0;
CREATE TABLE pid_table(pid_no INT PRIMARY KEY);
--let $pid_file=`SELECT @@pid_file`
--replace_result $pid_file pid_file
--eval LOAD DATA LOCAL INFILE '$pid_file' INTO TABLE pid_table
--let $server_pid=`SELECT pid_no FROM pid_table`
DROP TABLE pid_table;

# Supress log errors since they are expected.
call mtr.add_suppression("Member was expelled from the group due to network failures, changing member status to ERROR.");
call mtr.add_suppression("The server was automatically set into read only mode after an error was detected.");
call mtr.add_suppression("The server was automatically set into offline mode after an error was detected.");
call mtr.add_suppression("Skipping leave operation: concurrent attempt to leave the group is on-going.");
call mtr.add_suppression("The plugin encountered a critical error and will abort: Member was expelled from the group due to network failures.");
call mtr.add_suppression("Error, maximum number of retries exceeded when waiting for the internal server session state to be operating");
call mtr.add_suppression("Failed to establish an internal server connection to execute plugin operations");
call mtr.add_suppression("Unable to confirm whether the server has left the group or not.*");
call mtr.add_suppression(".* read failed");
call mtr.add_suppression("\\[GCS\\] Unable to resolve peer address.");
SET SESSION sql_log_bin = 1;

--let $member1_uuid= `SELECT @@GLOBAL.server_uuid`
--let $local_address_server1= `SELECT @@GLOBAL.group_replication_local_address`
--let $group_seeds_server1= `SELECT @@GLOBAL.group_replication_group_seeds`

# Set the exit state action sysvar to READ_ONLY
SET @@GLOBAL.group_replication_exit_state_action = READ_ONLY;

--echo
--echo #########################################################################
--echo # 1) Force expel of member 1.
--echo #########################################################################
--echo
# Then we send a SIGSTOP to it. This will stop the server from sending the
# keep-alive message and thus it will be viewed as a faulty node by the rest of
# the group (and consequently expelled).
--exec kill -19 $server_pid

# Wait until the group settles on 2 members
--let $rpl_connection_name= server2
--source include/connection.inc
--let $group_replication_number_of_members= 2
--source include/gr_wait_for_number_of_members.inc

--let $rpl_connection_name= server3
--source include/connection.inc
--let $group_replication_number_of_members= 2
--source include/gr_wait_for_number_of_members.inc

--echo
--echo #########################################################################
--echo # 1.1) Verify that member 1 went to super_read_only mode.
--echo #########################################################################
--echo
# Send SIGCONT to server, so it can continue (now that he is expelled)
--let $rpl_connection_name= server1
--source include/connection.inc
--exec kill -18 $server_pid

# Verify that the member entered an error state
--let $group_replication_member_state= ERROR
--let $group_replication_member_id= $member1_uuid
--source include/gr_wait_for_member_state.inc

# Then verify that it enabled super_read_only
--let $rpl_connection_name= server1
--source include/connection.inc
--let $assert_text= super_read_only should be enabled
--let $assert_cond= [SELECT @@GLOBAL.super_read_only] = 1;
--source include/assert.inc
--source include/stop_group_replication.inc

--echo
--echo #########################################################################
--echo # 2) Set exit state action to ABORT_SERVER on member 1.
--echo #########################################################################
--echo
# Set the exit state action sysvar to ABORT_SERVER
SET @@GLOBAL.group_replication_exit_state_action = ABORT_SERVER;
--source include/start_group_replication.inc
--let $group_replication_number_of_members = 3
--source include/gr_wait_for_number_of_members.inc

--echo
--echo #########################################################################
--echo # 3) Force expel again of member 1.
--echo #########################################################################
--echo
# Inform MTR that we are expecting an abort and that it should wait before
# restarting the aborting member
--exec echo "wait" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
# Send SIGSTOP again
--exec kill -19 $server_pid
# Wait until the group settles on 2 members
--let $rpl_connection_name= server2
--source include/connection.inc
--let $group_replication_number_of_members= 2
--source include/gr_wait_for_number_of_members.inc
--let $rpl_connection_name= server3
--source include/connection.inc
--let $group_replication_number_of_members= 2
--source include/gr_wait_for_number_of_members.inc

--echo
--echo #########################################################################
--echo # 3.1) Wait until server is aborted.
--echo #########################################################################
--echo
# Send SIGCONT to server, so it can continue (now that he is expelled)
--let $rpl_connection_name= server1
--source include/connection.inc
--exec kill -18 $server_pid
# Wait until the connection is dropped
--source include/wait_until_disconnected.inc

--echo
--echo #########################################################################
--echo # 4) Relaunch member 1.
--echo #########################################################################
--echo
--exec echo "restart" > $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
--let $rpl_server_number= 1
--source include/rpl/reconnect.inc

--let $rpl_connection_name= server1
--source include/connection.inc
--replace_result $group_seeds_server1 GROUP_SEEDS_SERVER1
--eval SET @@global.group_replication_group_seeds="$group_seeds_server1"
--replace_result $local_address_server1 LOCAL_ADDRESS_SERVER1
--eval SET @@global.group_replication_local_address="$local_address_server1"
--source include/start_group_replication.inc
# Wait for member to stabilize in the group
--let $group_replication_number_of_members = 3
--source include/gr_wait_for_number_of_members.inc

--echo
--echo #########################################################################
--echo # 5) Cleanup.
--echo #########################################################################
--echo
--source include/group_replication_end.inc
