import redis
from functools import wraps
import multiprocessing as mp
from includes import *

'''
python -m RLTest --test tests_sanitizer.py --module path/to/redisai.so
'''


def test_sanitizer_dagrun_mobilenet_v1(env):
    if (not TEST_TF or not TEST_PT):
        return
    con = env.getConnection()
    mem_allocator = con.info()['mem_allocator']
    if 'jemalloc' in mem_allocator:
        print("exiting sanitizer tests given we're not using stdlib allocator")
        return

    model_name = 'mobilenet_v1{s}'
    model_pb, input_var, output_var, labels, img = load_mobilenet_v1_test_data()

    ret = con.execute_command('AI.MODELSTORE', model_name, 'TF', DEVICE,
                              'INPUTS', 1, input_var,
                              'OUTPUTS', 1, output_var,
                              'BLOB', model_pb)
    env.assertEqual(ret, b'OK')

    for opnumber in range(1, MAX_ITERATIONS):
        image_key = 'image{{s}}{}'.format(opnumber)
        class_key = 'output{s}'

        ret = con.execute_command(
            'AI.DAGRUN', '|>',
            'AI.TENSORSET', image_key, 'FLOAT', 1, 224, 224, 3, 'BLOB', img.tobytes(),
            '|>',
            'AI.MODELRUN', model_name,
                         'INPUTS', image_key,
                         'OUTPUTS', class_key,
                          '|>',
            'AI.TENSORGET',  class_key, 'blob'
        )
        env.assertEqual([b'OK', b'OK'], ret[:2])
        env.assertEqual(1001.0, len(ret[2])/4)


def test_sanitizer_modelrun_mobilenet_v1(env):
    if (not TEST_TF or not TEST_PT):
        return
    con = env.getConnection()
    mem_allocator = con.info()['mem_allocator']
    if 'jemalloc' in mem_allocator:
        print("exiting sanitizer tests given we're not using stdlib allocator")
        return

    model_name = 'mobilenet_v1{s}'
    model_pb, input_var, output_var, labels, img = load_mobilenet_v1_test_data()

    ret = con.execute_command('AI.MODELSTORE', model_name, 'TF', DEVICE,
                              'INPUTS', 1, input_var,
                              'OUTPUTS', 1, output_var,
                              'BLOB', model_pb)
    env.assertEqual(ret, b'OK')

    for opnumber in range(1, MAX_ITERATIONS):
        image_key = 'image{s}'
        temp_key1 = 'temp_key1{s}'
        temp_key2 = 'temp_key2{s}'
        class_key = 'output{s}'
        ret = con.execute_command(
            'AI.TENSORSET', image_key, 'FLOAT', 1, 224, 224, 3, 'BLOB', img.tobytes()
        )
        env.assertEqual(b'OK', ret)

        ret = con.execute_command(
            'AI.MODELEXECUTE', model_name,
            'INPUTS', 1, image_key,
            'OUTPUTS', 1, class_key
        )

        env.assertEqual(b'OK', ret)

        ret = con.execute_command('AI.TENSORGET',  class_key, 'blob')

        env.assertEqual(1001.0, len(ret)/4)
