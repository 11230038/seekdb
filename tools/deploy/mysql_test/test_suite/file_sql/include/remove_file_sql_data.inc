# Remove only the fixture directory created by copy_file_sql_data.inc.
connect (file_sql_cleanup,$OBMYSQL_MS0,$OBMYSQL_USR,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_cleanup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_DATA_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql;
let $FILE_SQL_CLEANUP_CMD = ([ "$FILE_SQL_HOST" = "127.0.0.1" ] || [ "$FILE_SQL_HOST" = "localhost" ] || [ "$FILE_SQL_HOST" = "::1" ]) && (rm -rf "$FILE_SQL_DATA_DIR") || (ssh $FILE_SQL_HOST 'rm -rf "$FILE_SQL_DATA_DIR"');
exec $FILE_SQL_CLEANUP_CMD;
--enable_query_log

disconnect file_sql_cleanup;
connection default;
