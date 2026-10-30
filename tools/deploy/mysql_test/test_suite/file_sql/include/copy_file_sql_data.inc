# Install deterministic file-table fixtures in the observer working directory.
connect (file_sql_setup,$OBMYSQL_MS0,$OBMYSQL_USR,$OBMYSQL_PWD,test,$OBMYSQL_PORT);
connection file_sql_setup;

--disable_query_log
let $FILE_SQL_HOST = query_get_value('select host_ip()',host_ip(), 1);
let $FILE_SQL_DATA_DIR = $OBSERVER_DIR/data_for_mysqltest_file_sql;
let $FILE_SQL_COPY_CMD = ([ "$FILE_SQL_HOST" = "127.0.0.1" ] || [ "$FILE_SQL_HOST" = "localhost" ] || [ "$FILE_SQL_HOST" = "::1" ]) && (rm -rf "$FILE_SQL_DATA_DIR" && mkdir -p "$FILE_SQL_DATA_DIR" && cp $PWD/mysql_test/test_suite/file_sql/data/* "$FILE_SQL_DATA_DIR/" && base64 --decode --ignore-garbage "$FILE_SQL_DATA_DIR/simple.parquet.b64" > "$FILE_SQL_DATA_DIR/simple.parquet" && rm -f "$FILE_SQL_DATA_DIR/simple.parquet.b64") || (ssh $FILE_SQL_HOST 'rm -rf "$FILE_SQL_DATA_DIR" && mkdir -p "$FILE_SQL_DATA_DIR"' && scp -q $PWD/mysql_test/test_suite/file_sql/data/* $FILE_SQL_HOST:"$FILE_SQL_DATA_DIR/" && ssh $FILE_SQL_HOST 'base64 --decode --ignore-garbage "$FILE_SQL_DATA_DIR/simple.parquet.b64" > "$FILE_SQL_DATA_DIR/simple.parquet" && rm -f "$FILE_SQL_DATA_DIR/simple.parquet.b64"');
exec $FILE_SQL_COPY_CMD;
--enable_query_log

disconnect file_sql_setup;
connection default;
