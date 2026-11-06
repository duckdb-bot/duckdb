# fmt: off

import gzip
import pytest
import subprocess
import sys
from typing import List
from conftest import ShellTest
from conftest import json_extension
import os
import re

@pytest.mark.skipif(os.name == 'nt', reason="Skipped on windows")
class TestReadFromStdin(object):

    def test_read_stdin_direct(self, shell):
        test = (
            ShellTest(shell)
            .input_file('data/csv/test/test.csv')
            .statement("""create table mytable as select * from read_text('/dev/stdin')""")
            .statement("select length(content) as len from mytable;")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout("len")
        result.check_stdout('77780')

    def test_read_stdin_csv(self, shell):
        test = (
            ShellTest(shell)
            .input_file('data/csv/test/test.csv')
            .statement("""
                create table mytable as select * from
                read_csv('/dev/stdin',
                    columns=STRUCT_PACK(foo := 'INTEGER', bar := 'INTEGER', baz := 'VARCHAR'),
                    AUTO_DETECT='false'
                )
            """)
            .statement("select * from mytable limit 1;")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout("foo,bar,baz")
        result.check_stdout('0,0, test')

    def test_read_stdin_csv_where_filename(self, shell):
        test = (
            ShellTest(shell)
            .input_file('data/csv/test/test.csv')
            .statement("""
                SELECT * FROM read_csv_auto(
                    'data/csv/bug_9005/teste*.csv',
                    header=TRUE,
                    filename=true,
                    union_by_name=True
                ) where filename='data/csv/bug_9005/teste1.csv'
            """)
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout([
            'id,name,factor,filename',
            '1,Ricardo,1.5,data/csv/bug_9005/teste1.csv',
            '2,Jose,2.0,data/csv/bug_9005/teste1.csv'
        ])

    def test_read_stdin_csv_auto(self, shell):
        test = (
            ShellTest(shell)
            .input_file('data/csv/test/test.csv')
            .statement("""
                create table mytable as select * from
                read_csv_auto('/dev/stdin')
            """)
            .statement("select * from mytable limit 1;")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout("column0,column1,column2")
        result.check_stdout('0,0, test')

    @pytest.mark.parametrize("reader,expected", [
        ("read_csv('/dev/stdin', columns = {'time': 'DOUBLE'})", '2,1.0,2.0'),
        ("read_csv_auto('/dev/stdin', header = false)", '2,1,2'),
    ])
    def test_summarize_stdin_csv(self, shell, tmp_path, reader, expected):
        input_file = tmp_path / 'summarize.csv'
        input_file.write_text('1\n2\n')
        test = (
            ShellTest(shell)
            .input_file(input_file)
            .statement(f"SELECT count, min, max FROM (SUMMARIZE (FROM {reader}))")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout('count,min,max')
        result.check_stdout(expected)

    def test_split_part_csv(self, shell):
        test = (
            ShellTest(shell)
            .input_file('data/csv/split_part.csv')
            .statement("""
                FROM read_csv('/dev/stdin') select split_part(C1, ',', 2) as res;
            """)
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout("res")
        result.check_stdout('12')
        result.check_stdout('12')

    def test_read_stdin_csv_auto_projection(self, shell):
        test = (
            ShellTest(shell)
            .input_file('data/csv/tpcds_14.csv')
            .statement("""
                create table mytable as select * from
                read_csv_auto('/dev/stdin')
            """)
            .statement("select channel,i_brand_id,sum_sales,number_sales from mytable;")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout("web,8006004,844.21,21")

    @staticmethod
    def scan_estimate(shell, input_file, source, options=''):
        test = (
            ShellTest(shell)
            .input_file(input_file)
            .statement(f"EXPLAIN SELECT * FROM read_csv('{source}'{options})")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        assert result.status_code == 0
        estimates = re.findall(r'~([0-9,]+) rows', result.stdout)
        assert len(estimates) == 1
        return int(estimates[0].replace(',', ''))

    # the sniffer reads all of this input, in one buffer or in many, so the pipe is estimated like the file
    @pytest.mark.parametrize("options", [
        '',
        ', buffer_size = 8192, sample_size = -1'
    ])
    def test_read_stdin_csv_cardinality(self, shell, options):
        input_file = 'data/csv/test/test.csv'
        pipe_estimate = self.scan_estimate(shell, input_file, '/dev/stdin', options)
        file_estimate = self.scan_estimate(shell, input_file, input_file, options)
        assert pipe_estimate == file_estimate

    def test_read_stdin_csv_cardinality_partial(self, shell):
        # the sniffer reads part of this input, so the pipe is estimated from at least one buffer
        input_file = 'data/csv/tpcds_59.csv'
        options = ', sample_size = 1024, buffer_size = 262144'
        pipe_estimate = self.scan_estimate(shell, input_file, '/dev/stdin', options)
        file_estimate = self.scan_estimate(shell, input_file, input_file, options)
        # ten columns of an estimated five bytes each
        assert 262144 // 50 <= pipe_estimate < file_estimate

    def test_read_stdin_csv_semi_join_memory(self, shell):
        # the piped input does not fit in memory, so it has to be the probe side of the join
        input_data = 'id,payload\n' + ''.join(f'{i},{"x" * 100}\n' for i in range(100000))
        test = (
            ShellTest(shell)
            .add_argument(
                '-csv',
                '-c',
                """
                SET threads = 2;
                SET memory_limit = '8MB';
                SELECT count(*), max(payload) = repeat('x', 100) FROM read_csv('/dev/stdin', buffer_size = 1048576)
                WHERE id IN (SELECT range FROM range(100));
                """
            )
        )
        result = test.run_raw(input_data)
        result.check_stdout('100,true')


    def test_read_stdin_ndjson(self, shell, json_extension):
        test = (
            ShellTest(shell)
            .input_file('data/json/example_rn.ndjson')
            .statement("""
                create table mytable as select * from
                read_ndjson_objects('/dev/stdin')
            """)
            .statement("select * from mytable;")
            .add_argument(
                '-list',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout([
            "json",
            '{"id":1,"name":"O Brother, Where Art Thou?"}',
            '{"id":2,"name":"Home for the Holidays"}',
            '{"id":3,"name":"The Firm"}',
            '{"id":4,"name":"Broadcast News"}',
            '{"id":5,"name":"Raising Arizona"}'
        ])


    def test_read_stdin_json_auto(self, shell, json_extension):
        test = (
            ShellTest(shell)
            .input_file('data/json/example_rn.ndjson')
            .statement("""
                create table mytable as select * from
                read_json_auto('/dev/stdin')
            """)
            .statement("select * from mytable;")
            .add_argument(
                '-list',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout([
            'id|name',
            '1|O Brother, Where Art Thou?',
            '2|Home for the Holidays',
            '3|The Firm',
            '4|Broadcast News',
            '5|Raising Arizona'
        ])

    def test_read_stdin_json_array(self, shell, json_extension):
        test = (
            ShellTest(shell)
            .input_file('data/json/11407.json')
            .statement("""
                create table mytable as select * from
                read_json_auto('/dev/stdin')
            """)
            .statement("select * from mytable;")
            .add_argument(
                '-list',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout([
            'k',
            'v',
            'v2'
        ])

    def test_read_stdin_json_auto_recursive_cte(self, shell, json_extension):
        # FIXME: disabled for now
        return
        test = (
            ShellTest(shell)
            .input_file('data/json/filter_keystage.ndjson')
            .statement("""
                CREATE TABLE mytable AS
                WITH RECURSIVE nums AS (
                    SELECT 0 AS n
                UNION ALL
                    SELECT n + 1
                    FROM nums
                    WHERE n < (
                        SELECT MAX(JSON_ARRAY_LENGTH(filter_keystage))::int - 1
                        FROM read_json_auto('/dev/stdin'))
                ) SELECT * FROM nums;
            """)
            .statement("select * from mytable;")
            .add_argument(
                '-list',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout([
            'n',
            '0',
            '1',
            '2',
        ])


    @pytest.mark.parametrize("alias", [
        "'/dev/stdout'",
        'stdout'
    ])
    def test_copy_to_stdout(self, shell, alias):
        test = (
            ShellTest(shell)
            .statement(f"COPY (SELECT 42) TO {alias};")
        )
        result = test.run()
        result.check_stdout('42')


    @pytest.mark.parametrize("alias", [
        "'/dev/stdout'",
        'stdout'
    ])
    def test_copy_csv_to_stdout(self, shell, alias):
        test = (
            ShellTest(shell)
            .statement(f"COPY (SELECT 42) TO {alias} WITH (FORMAT 'csv');")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout('42')


    @pytest.mark.parametrize("alias", [
        "'/dev/stdin'",
        'stdin',
        'STDIN'
    ])
    def test_copy_from_stdin(self, shell, tmp_path, alias):
        input_file = tmp_path / 'copy_from_stdin.csv'
        input_file.write_text('1,2\n3,4\n')
        test = (
            ShellTest(shell)
            .input_file(input_file)
            .statement("CREATE TABLE t(a INTEGER, b INTEGER)")
            .statement(f"COPY t FROM {alias}")
            .statement("SELECT * FROM t ORDER BY a")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stdout([
            'a,b',
            '1,2',
            '3,4'
        ])


    @pytest.mark.parametrize("alias", [
        "'/dev/stderr'"
    ])
    def test_copy_csv_to_stderr(self, shell, alias):
        test = (
            ShellTest(shell)
            .statement(f"COPY (SELECT 42) TO {alias} WITH (FORMAT 'csv');")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stderr('42')


    def test_copy_non_inlined_string(self, shell):
        test = (
            ShellTest(shell)
            .statement("select list(concat('thisisalongstring', range::VARCHAR)) i from range(10000)")
        )
        result = test.run()
        result.check_stdout('thisisalongstring')


    def test_write_to_stdout_piped_to_file(self, shell, random_filepath):
        test = (
            ShellTest(shell)
            .statement("copy (select * from range(10000) tbl(i)) to '/dev/stdout' (format csv)")
            .output_file(random_filepath.as_posix())
        )
        result = test.run()
        result.check_stdout('9999')

    @pytest.mark.parametrize("compression", ['gzip', 'zstd'])
    def test_read_compressed_stdin_csv(self, shell, tmp_path, compression):
        input_file = tmp_path / 'compressed.csv.gz'
        input_file.write_bytes(gzip.compress(b'id\n1\n2\n'))
        test = (
            ShellTest(shell)
            .input_file(input_file)
            .statement(f"SELECT count(*) FROM read_csv('/dev/stdin', compression = '{compression}')")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stderr('Cannot read compressed data from the pipe "/dev/stdin"')

    def test_read_compressed_stdin_json(self, shell, tmp_path, json_extension):
        input_file = tmp_path / 'compressed.json.gz'
        input_file.write_bytes(gzip.compress(b'{"id": 1}\n{"id": 2}\n'))
        test = (
            ShellTest(shell)
            .input_file(input_file)
            .statement("SELECT count(*) FROM read_json('/dev/stdin', compression = 'gzip')")
            .add_argument(
                '-csv',
                ':memory:'
            )
        )
        result = test.run()
        result.check_stderr('Cannot read compressed data from the pipe "/dev/stdin"')

    def run_shell(self, shell, sql):
        return subprocess.run(
            [shell, '--no-init', '-c', sql],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=ShellTest(shell).get_environment(),
            timeout=30,
        )

    def test_copy_compressed_csv_to_stdout(self, shell, tmp_path):
        # the compression option applies to a pipe as it does to a file
        res = self.run_shell(shell, "COPY (SELECT 42 AS x) TO '/dev/stdout' (FORMAT csv, COMPRESSION gzip)")
        assert res.returncode == 0
        assert gzip.decompress(res.stdout) == b'x\n42\n'
        res = self.run_shell(shell, "COPY (SELECT 42 AS x) TO '/dev/stdout' (FORMAT csv, COMPRESSION zstd)")
        assert res.returncode == 0
        zstd_file = tmp_path / 'out.csv.zst'
        zstd_file.write_bytes(res.stdout)
        res = self.run_shell(shell, f"SELECT x FROM read_csv('{zstd_file.as_posix()}')")
        assert res.returncode == 0
        assert b'42' in res.stdout

    @pytest.mark.parametrize("name, compression, expected", [
        ('named.csv.gz', '', None),
        ('named.csv.gz', ', COMPRESSION uncompressed', b'x\n42\n'),
        # a suffix whose compression has no filesystem is written as is, as for a file
        ('named.csv.xz', '', b'x\n42\n'),
    ])
    def test_copy_to_named_compressed_fifo(self, shell, tmp_path, name, compression, expected):
        # the suffix of a named pipe selects the compression as it does for a file
        fifo = tmp_path / name
        os.mkfifo(fifo)
        # an open read end lets the shell open the pipe for writing, the small output stays in the pipe buffer
        read_fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        try:
            res = self.run_shell(shell, f"COPY (SELECT 42 AS x) TO '{fifo.as_posix()}' (FORMAT csv{compression})")
            received = b''
            while True:
                try:
                    chunk = os.read(read_fd, 65536)
                except BlockingIOError:
                    break
                if not chunk:
                    break
                received += chunk
        finally:
            os.close(read_fd)
        assert res.returncode == 0
        if expected is None:
            assert gzip.decompress(received) == b'x\n42\n'
        else:
            assert received == expected

    @pytest.mark.parametrize("name, error", [
        ('named.csv.gz', b'Cannot read compressed data from the pipe'),
        # a suffix whose compression has no filesystem is refused, as for a file
        ('named.csv.bz2', b'compression type is not supported (compression type "bzip2")'),
    ])
    def test_read_named_compressed_fifo(self, shell, tmp_path, name, error):
        fifo = tmp_path / name
        os.mkfifo(fifo)
        # an open write end lets the shell open the pipe for reading, the data waits in the pipe buffer
        read_fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        write_fd = os.open(fifo, os.O_WRONLY)
        try:
            os.write(write_fd, gzip.compress(b'id\n1\n2\n'))
            res = self.run_shell(shell, f"SELECT count(*) FROM read_csv('{fifo.as_posix()}')")
        finally:
            os.close(write_fd)
            os.close(read_fd)
        assert error in res.stderr

# fmt: on
