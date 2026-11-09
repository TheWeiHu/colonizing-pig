"""Print a deployable CloudFormation template for the replay collector."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
digest = hashlib.sha256()
for path in sorted(ROOT.joinpath('engine').iterdir()):
    if path.suffix in ('.cpp', '.hpp'):
        digest.update(path.name.encode() + b'\0' + path.read_bytes() + b'\0')
ENGINE_HASH = digest.hexdigest()
MODEL_HASH = hashlib.sha256((ROOT / 'model.json').read_bytes()).hexdigest()
ref = lambda name: {'Ref': name}
sub = lambda value: {'Fn::Sub': value}
attr = lambda name, key: {'Fn::GetAtt': [name, key]}

template = {
    'AWSTemplateFormatVersion': '2010-09-09',
    'Description': 'Private, bounded storage for unverified Colonizing Pig browser replays.',
    'Resources': {
        'ReplayBucket': {'Type': 'AWS::S3::Bucket', 'DeletionPolicy': 'Retain', 'UpdateReplacePolicy': 'Retain', 'Properties': {
            'PublicAccessBlockConfiguration': {'BlockPublicAcls': True, 'BlockPublicPolicy': True, 'IgnorePublicAcls': True, 'RestrictPublicBuckets': True},
            'OwnershipControls': {'Rules': [{'ObjectOwnership': 'BucketOwnerEnforced'}]},
            'BucketEncryption': {'ServerSideEncryptionConfiguration': [{'ServerSideEncryptionByDefault': {'SSEAlgorithm': 'AES256'}}]}}},
        'BucketPolicy': {'Type': 'AWS::S3::BucketPolicy', 'Properties': {'Bucket': ref('ReplayBucket'), 'PolicyDocument': {
            'Version': '2012-10-17', 'Statement': [{'Effect': 'Deny', 'Principal': '*', 'Action': 's3:*',
                'Resource': [attr('ReplayBucket', 'Arn'), sub('${ReplayBucket.Arn}/*')], 'Condition': {'Bool': {'aws:SecureTransport': 'false'}}}]}}},
        'DailyQuota': {'Type': 'AWS::DynamoDB::Table', 'Properties': {'BillingMode': 'PAY_PER_REQUEST',
            'AttributeDefinitions': [{'AttributeName': 'day', 'AttributeType': 'S'}],
            'KeySchema': [{'AttributeName': 'day', 'KeyType': 'HASH'}],
            'TimeToLiveSpecification': {'AttributeName': 'expires', 'Enabled': True}}},
        'CollectorLogs': {'Type': 'AWS::Logs::LogGroup', 'Properties': {'LogGroupName': '/aws/lambda/colonizing-pig-replays', 'RetentionInDays': 7}},
        'CollectorRole': {'Type': 'AWS::IAM::Role', 'Properties': {
            'AssumeRolePolicyDocument': {'Version': '2012-10-17', 'Statement': [{'Effect': 'Allow', 'Principal': {'Service': 'lambda.amazonaws.com'}, 'Action': 'sts:AssumeRole'}]},
            'Policies': [{'PolicyName': 'ReplayStorage', 'PolicyDocument': {'Version': '2012-10-17', 'Statement': [
                {'Effect': 'Allow', 'Action': ['s3:PutObject'], 'Resource': sub('${ReplayBucket.Arn}/unverified/*')},
                {'Effect': 'Allow', 'Action': ['dynamodb:UpdateItem'], 'Resource': attr('DailyQuota', 'Arn')},
                {'Effect': 'Allow', 'Action': ['logs:CreateLogStream', 'logs:PutLogEvents'],
                 'Resource': sub('arn:${AWS::Partition}:logs:${AWS::Region}:${AWS::AccountId}:log-group:/aws/lambda/colonizing-pig-replays:*')}
            ]}}]}},
        'Collector': {'Type': 'AWS::Lambda::Function', 'DependsOn': 'CollectorLogs', 'Properties': {
            'FunctionName': 'colonizing-pig-replays', 'Runtime': 'python3.12', 'Handler': 'index.handler',
            'Architectures': ['arm64'], 'MemorySize': 128, 'Timeout': 10,
            'Role': attr('CollectorRole', 'Arn'), 'Code': {'ZipFile': (ROOT / 'backend/collector.py').read_text()},
            'Environment': {'Variables': {'DATA_BUCKET': ref('ReplayBucket'), 'QUOTA_TABLE': ref('DailyQuota'),
                'DAILY_LIMIT': '20000', 'ENGINE_HASH': ENGINE_HASH, 'MODEL_HASH': MODEL_HASH}}}},
        'Api': {'Type': 'AWS::ApiGatewayV2::Api', 'Properties': {'Name': 'colonizing-pig-replays', 'ProtocolType': 'HTTP',
            'CorsConfiguration': {'AllowOrigins': ['https://colonizing-pig.weihu.ca'], 'AllowMethods': ['POST', 'OPTIONS'], 'AllowHeaders': ['content-type'], 'ExposeHeaders': ['retry-after'], 'MaxAge': 86400}}},
        'Integration': {'Type': 'AWS::ApiGatewayV2::Integration', 'Properties': {'ApiId': ref('Api'), 'IntegrationType': 'AWS_PROXY',
            'IntegrationUri': attr('Collector', 'Arn'), 'PayloadFormatVersion': '2.0', 'TimeoutInMillis': 10000}},
        'Route': {'Type': 'AWS::ApiGatewayV2::Route', 'Properties': {'ApiId': ref('Api'), 'RouteKey': 'POST /games', 'Target': sub('integrations/${Integration}')}},
        'Stage': {'Type': 'AWS::ApiGatewayV2::Stage', 'Properties': {'ApiId': ref('Api'), 'StageName': '$default', 'AutoDeploy': True,
            'DefaultRouteSettings': {'ThrottlingRateLimit': 2, 'ThrottlingBurstLimit': 20}}},
        'InvokePermission': {'Type': 'AWS::Lambda::Permission', 'Properties': {'FunctionName': ref('Collector'),
            'Action': 'lambda:InvokeFunction', 'Principal': 'apigateway.amazonaws.com',
            'SourceArn': sub('arn:${AWS::Partition}:execute-api:${AWS::Region}:${AWS::AccountId}:${Api}/*/POST/games')}}
    },
    'Outputs': {'Endpoint': {'Value': sub('https://${Api}.execute-api.${AWS::Region}.${AWS::URLSuffix}/games')},
        'Bucket': {'Value': ref('ReplayBucket')}, 'EngineHash': {'Value': ENGINE_HASH}, 'ModelHash': {'Value': MODEL_HASH}}
}

if __name__ == '__main__':
    print(json.dumps(template, indent=2))
