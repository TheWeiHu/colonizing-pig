import base64
import copy
import gzip
import importlib.util
import json
import os
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('collector', Path(__file__).resolve().parents[1] / 'backend/collector.py')
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)
ENGINE, MODEL = 'a' * 64, 'b' * 64


class AwsError(Exception):
    def __init__(self, code):
        self.response = {'Error': {'Code': code}}


class Store:
    def __init__(self):
        self.objects = {}

    def put_object(self, **kwargs):
        assert kwargs['IfNoneMatch'] == '*'
        assert kwargs['ServerSideEncryption'] == 'AES256'
        if kwargs['Key'] in self.objects:
            raise AwsError('PreconditionFailed')
        self.objects[kwargs['Key']] = kwargs


class Quota:
    def __init__(self):
        self.count = 0
        self.full = False

    def update_item(self, **kwargs):
        assert kwargs['ConditionExpression'] == 'attribute_not_exists(#n) OR #n < :limit'
        if self.full:
            raise AwsError('ConditionalCheckFailedException')
        self.count += 1


class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.data = {'version': 1, 'engine': ENGINE, 'model': MODEL, 'seed': 17, 'human': 0,
                     'moves': [0, 54, 248], 'result': 'completed', 'winner': 1}
        self.store, self.quota = Store(), Quota()
        self.environment = patch.dict(os.environ, ENGINE_HASH=ENGINE, MODEL_HASH=MODEL,
                                      DATA_BUCKET='private', QUOTA_TABLE='quota', DAILY_LIMIT='20000')
        self.environment.start()
        self.service_patch = patch.object(collector, 'services', return_value=(self.store, self.quota))
        self.service_patch.start()
        self.addCleanup(self.environment.stop)
        self.addCleanup(self.service_patch.stop)

    def event(self, data=None):
        return {'headers': {'Origin': collector.ORIGIN, 'Content-Type': 'application/json'},
                'body': json.dumps(self.data if data is None else data),
                'requestContext': {'http': {'method': 'POST', 'sourceIp': 'not-stored'}}}

    def test_stores_compressed_unverified_replay_without_request_metadata(self):
        result = collector.handler(self.event(), None)
        self.assertEqual(result['statusCode'], 201)
        saved = json.loads(gzip.decompress(next(iter(self.store.objects.values()))['Body']))
        self.assertEqual(saved['replay'], self.data)
        self.assertEqual(saved['validation'], 'unverified')
        self.assertEqual(set(saved), {'receivedAt', 'validation', 'replay'})
        self.assertNotIn('not-stored', json.dumps(saved))

    def test_retry_and_json_key_order_are_idempotent(self):
        first = collector.handler(self.event(), None)
        second = collector.handler(self.event(dict(reversed(list(self.data.items())))), None)
        self.assertEqual(second['statusCode'], 200)
        self.assertEqual(first['body'], second['body'])
        self.assertEqual(len(self.store.objects), 1)

    def test_daily_cap_prevents_storage(self):
        self.quota.full = True
        result = collector.handler(self.event(), None)
        self.assertEqual(result['statusCode'], 429)
        self.assertIn('Retry-After', result['headers'])
        self.assertFalse(self.store.objects)

    def test_origin_and_method(self):
        event = self.event(); event['headers']['Origin'] = 'https://attacker.example'
        response = collector.handler(event, None)
        self.assertEqual(response['statusCode'], 403)
        self.assertNotIn('Access-Control-Allow-Origin', response['headers'])
        event = self.event(); event['requestContext']['http']['method'] = 'GET'
        self.assertEqual(collector.handler(event, None)['statusCode'], 405)
        self.assertEqual(self.quota.count, 0)

    def test_resign_before_first_move(self):
        data = {**self.data, 'result': 'resigned', 'moves': [], 'winner': 1}
        self.assertEqual(collector.handler(self.event(data), None)['statusCode'], 201)

    def test_rejects_invalid_fields_without_charging_quota(self):
        invalid = [{'seed': True}, {'seed': -1}, {'seed': 2**32}, {'human': True}, {'winner': True},
                   {'version': True}, {'version': 2}, {'model': 'c' * 64}, {'engine': ''},
                   {'moves': []}, {'moves': [249]}, {'moves': [True]}, {'moves': [0] * 10001},
                   {'result': 'resigned', 'winner': 0}, {'result': 'quit'}, {'extra': 'data'}]
        for change in invalid:
            with self.subTest(change=change):
                self.assertEqual(collector.handler(self.event({**self.data, **change}), None)['statusCode'], 400)
        self.assertEqual(self.quota.count, 0)

    def test_size_encoding_and_json_errors(self):
        event = self.event(); event['body'] = 'x' * 65537
        self.assertEqual(collector.handler(event, None)['statusCode'], 413)
        event = self.event(); event['body'] = '{bad'
        self.assertEqual(collector.handler(event, None)['statusCode'], 400)
        event = self.event(); event['body'] = base64.b64encode(event['body'].encode()).decode(); event['isBase64Encoded'] = True
        self.assertEqual(collector.handler(event, None)['statusCode'], 201)
        event['body'] = '***'
        self.assertEqual(collector.handler(event, None)['statusCode'], 400)

    def test_backend_error_is_retriable_and_not_exposed(self):
        with patch.object(self.store, 'put_object', side_effect=AwsError('SensitiveAwsDetail')):
            response = collector.handler(self.event(), None)
        self.assertEqual(response['statusCode'], 503)
        self.assertNotIn('SensitiveAwsDetail', response['body'])


if __name__ == '__main__':
    unittest.main()
