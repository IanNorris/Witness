"""Read-only ONVIF capability probe. Never issues a movement/recording command.

Credentials come from a read-only SQLite connection and never enter output or
command-line arguments. Report only model/capabilities, not credential material.
"""
import argparse
import base64
import datetime
import hashlib
import json
from pathlib import Path
import secrets
import sqlite3
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape
from test_onvif_ptz_transport import SOAP, DEVICE, MEDIA, PTZ, SCHEMA, WSSE, WSU

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database',type=Path,required=True)
    parser.add_argument('--camera',type=int,required=True)
    parser.add_argument('--host',help='Explicit camera host when RTSP uses a proxy')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists(): parser.error('Refusing to overwrite report')
    with sqlite3.connect(args.database.resolve().as_uri()+'?mode=ro',uri=True) as database:
        row=database.execute('SELECT CameraString,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword FROM Camera WHERE CameraUID=?',(args.camera,)).fetchone()
    if not row: parser.error('Camera not found')
    rtsp=urllib.parse.urlsplit(row[0]);host=args.host or row[1] or rtsp.hostname
    port=row[2] if row[2] and row[2] not in (80,443) else 2020
    user=row[3] or urllib.parse.unquote(rtsp.username or '')
    password=row[4] or urllib.parse.unquote(rtsp.password or '')
    report={'cameraId':args.camera,'readOnly':True,'operations':[]}
    def call(path,namespace,operation,parameters=''):
        nonce=secrets.token_bytes(16);created=datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
        digest=base64.b64encode(hashlib.sha1(nonce+created.encode()+password.encode()).digest()).decode()
        root='http://docs.oasis-open.org/wss/2004/01/'
        security=f'<w:Security s:mustUnderstand="1" xmlns:w="{WSSE}" xmlns:u="{WSU}"><w:UsernameToken><w:Username>{escape(user)}</w:Username><w:Password Type="{root}oasis-200401-wss-username-token-profile-1.0#PasswordDigest">{digest}</w:Password><w:Nonce EncodingType="{root}oasis-200401-wss-soap-message-security-1.0#Base64Binary">{base64.b64encode(nonce).decode()}</w:Nonce><u:Created>{created}</u:Created></w:UsernameToken></w:Security>'
        data=f'<s:Envelope xmlns:s="{SOAP}" xmlns:m="{namespace}"><s:Header>{security}</s:Header><s:Body><m:{operation}>{parameters}</m:{operation}></s:Body></s:Envelope>'.encode()
        request=urllib.request.Request(f'http://{host}:{port}{path}',data=data,headers={'Content-Type':f'application/soap+xml; charset=utf-8; action="{namespace}/{operation}"'})
        # No redirects and no fallback to cloud/device control protocols.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self,*a,**kw): return None
        with urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect()).open(request,timeout=5) as response:
            payload=response.read(1024*1024+1)
        if len(payload)>1024*1024 or b'<!DOCTYPE' in payload: raise ValueError('Invalid response')
        tree=ET.fromstring(payload);body=tree.find(f'{{{SOAP}}}Body')
        if body is None or len(body)!=1 or body[0].tag!=f'{{{namespace}}}{operation}Response': raise ValueError('Fault or missing acknowledgement')
        report['operations'].append(operation)
        return body[0]
    def path(address):
        value=urllib.parse.urlsplit(address).path
        if not value.startswith('/onvif/'): raise ValueError('Invalid service path')
        return value
    try:
        info=call('/onvif/device_service',DEVICE,'GetDeviceInformation')
        report['model']=info.findtext(f'{{{DEVICE}}}Model');report['firmware']=info.findtext(f'{{{DEVICE}}}FirmwareVersion')
        caps=call('/onvif/device_service',DEVICE,'GetCapabilities','<m:Category>All</m:Category>')
        media=path(caps.findtext(f'{{{DEVICE}}}Capabilities/{{{SCHEMA}}}Media/{{{SCHEMA}}}XAddr'))
        ptz=path(caps.findtext(f'{{{DEVICE}}}Capabilities/{{{SCHEMA}}}PTZ/{{{SCHEMA}}}XAddr'))
        profiles=call(media,MEDIA,'GetProfiles')
        configurations=[]
        for profile in profiles:
            config=profile.find(f'{{{SCHEMA}}}PTZConfiguration')
            if config is not None:
                options=call(ptz,PTZ,'GetConfigurationOptions',f'<m:ConfigurationToken>{escape(config.get("token"))}</m:ConfigurationToken>')
                presets=call(ptz,PTZ,'GetPresets',f'<m:ProfileToken>{escape(profile.get("token"))}</m:ProfileToken>')
                configurations.append({'profileToken':profile.get('token'),'optionsXml':ET.tostring(options,encoding='unicode'),'presetCount':len(presets)})
                break
        report['profiles']=configurations;report['success']=bool(configurations)
    except Exception as error:
        report['success']=False;report['errorType']=type(error).__name__
        if isinstance(error,urllib.error.HTTPError): report['httpStatus']=error.code
    with args.output.open('x',encoding='utf8') as output: json.dump(report,output,indent=2)
    print(json.dumps(report,indent=2))
    return 0 if report['success'] else 1

if __name__=='__main__': raise SystemExit(main())
