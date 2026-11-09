"""Accept bounded, unverified game replays into private storage."""
import base64
import gzip
import hashlib
import json
import os
import time
from datetime import datetime, timezone

ORIGIN = 'https://colonizing-pig.weihu.ca'
MAX_BYTES = 65536
FIELDS = {'version', 'engine', 'model', 'seed', 'human', 'moves', 'result', 'winner'}
_services = None


def validate(data, engine, model):
    if not isinstance(data, dict) or set(data) != FIELDS:
        raise ValueError('Invalid replay fields')
    integer = lambda value: type(value) is int
    if not integer(data['version']) or data['version'] != 1:
        raise ValueError('Unsupported replay version')
    if data['engine'] != engine or data['model'] != model:
        raise ValueError('Unsupported model or engine')
    if not integer(data['seed']) or not 0 <= data['seed'] <= 4294967295:
        raise ValueError('Invalid seed')
    if not integer(data['human']) or data['human'] not in (0, 1):
        raise ValueError('Invalid player')
    if not integer(data['winner']) or data['winner'] not in (-1, 0, 1):
        raise ValueError('Invalid winner')
    if data['result'] not in ('completed', 'resigned'):
        raise ValueError('Invalid result')
    moves = data['moves']
    if not isinstance(moves, list) or len(moves) > 10000:
        raise ValueError('Invalid move history')
    if any(not integer(move) or not 0 <= move <= 248 for move in moves):
        raise ValueError('Invalid move')
    if data['result'] == 'completed' and not moves:
        raise ValueError('Completed game has no moves')
    if data['result'] == 'resigned' and data['winner'] != 1 - data['human']:
        raise ValueError('Invalid resignation winner')
    return json.dumps(data, sort_keys=True, separators=(',', ':')).encode()


def services():
    global _services
    if _services is None:
        import boto3
        _services = (boto3.client('s3'), boto3.client('dynamodb'))
    return _services


def handler(event, context):
    headers = {key.lower(): value for key, value in (event.get('headers') or {}).items()}
    allowed = headers.get('origin') == ORIGIN

    def reply(status, payload=None, retry=None):
        response_headers = {'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Vary': 'Origin'}
        if allowed:
            response_headers['Access-Control-Allow-Origin'] = ORIGIN
        if retry:
            response_headers['Retry-After'] = str(retry)
        return {'statusCode': status, 'headers': response_headers, 'body': json.dumps(payload or {})}

    if not allowed:
        return reply(403, {'error': 'Origin not allowed'})
    method = event.get('requestContext', {}).get('http', {}).get('method')
    if method != 'POST':
        return reply(405, {'error': 'Method not allowed'})
    if headers.get('content-type', '').split(';', 1)[0].strip().lower() != 'application/json':
        return reply(415, {'error': 'JSON required'})
    body = event.get('body') or ''
    if not isinstance(body, str) or len(body) > MAX_BYTES * 4 // 3 + 4:
        return reply(413, {'error': 'Replay too large'})
    try:
        raw = base64.b64decode(body, validate=True) if event.get('isBase64Encoded') else body.encode()
        if len(raw) > MAX_BYTES:
            return reply(413, {'error': 'Replay too large'})
        data = json.loads(raw)
        canonical = validate(data, os.environ['ENGINE_HASH'], os.environ['MODEL_HASH'])
    except (ValueError, TypeError, UnicodeError, RecursionError):
        return reply(400, {'error': 'Invalid replay'})
    digest = hashlib.sha256(canonical).hexdigest()
    now = datetime.now(timezone.utc)
    s3, dynamo = services()
    try:
        dynamo.update_item(
            TableName=os.environ['QUOTA_TABLE'], Key={'day': {'S': now.date().isoformat()}},
            UpdateExpression='SET #n = if_not_exists(#n, :zero) + :one, expires = if_not_exists(expires, :expiry)',
            ConditionExpression='attribute_not_exists(#n) OR #n < :limit',
            ExpressionAttributeNames={'#n': 'count'},
            ExpressionAttributeValues={':zero': {'N': '0'}, ':one': {'N': '1'},
                ':limit': {'N': os.environ.get('DAILY_LIMIT', '20000')},
                ':expiry': {'N': str(int(time.time()) + 172800)}})
    except Exception as error:
        code = getattr(error, 'response', {}).get('Error', {}).get('Code')
        if code == 'ConditionalCheckFailedException':
            return reply(429, {'error': 'Daily upload limit reached'}, retry=3600)
        return reply(503, {'error': 'Storage temporarily unavailable'}, retry=60)
    record = {'validation': 'unverified', 'receivedAt': now.isoformat(), 'replay': data}
    try:
        s3.put_object(Bucket=os.environ['DATA_BUCKET'], Key=f'unverified/{data["engine"]}/{digest}.json.gz',
            Body=gzip.compress(json.dumps(record, separators=(',', ':')).encode(), mtime=0),
            ContentType='application/json', ContentEncoding='gzip', ServerSideEncryption='AES256', IfNoneMatch='*')
    except Exception as error:
        code = getattr(error, 'response', {}).get('Error', {}).get('Code')
        if code == 'PreconditionFailed':
            return reply(200, {'id': digest, 'saved': True})
        return reply(503, {'error': 'Storage temporarily unavailable'}, retry=60)
    return reply(201, {'id': digest, 'saved': True})
