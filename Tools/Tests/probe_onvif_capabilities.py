"""ONVIF capability probe, read-only by default. Never displaces the camera.

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
    parser.add_argument('--clock-only',action='store_true',help='Unauthenticated read-only clock query')
    parser.add_argument('--soap11',action='store_true',help='Read-only SOAP 1.1 compatibility diagnosis')
    parser.add_argument('--camera-clock',action='store_true',help='Read and compensate device clock for WS-Security')
    parser.add_argument('--stationary-move-check',action='store_true',help='Check ContinuousMove acknowledgement with zero velocity (no camera displacement)')
    parser.add_argument('--move-timeout',default='PT10.000000S',choices=['PT10.000000S','PT10S'],help='Stationary diagnostic timeout format')
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
    report={'cameraId':args.camera,'readOnly':not args.stationary_move_check,'operations':[]}
    clock_offset=datetime.timedelta()
    def call(path,namespace,operation,parameters=''):
        soap_namespace='http://schemas.xmlsoap.org/soap/envelope/' if args.soap11 else SOAP
        nonce=secrets.token_bytes(16);created=(datetime.datetime.now(datetime.timezone.utc)+clock_offset).strftime('%Y-%m-%dT%H:%M:%SZ')
        digest=base64.b64encode(hashlib.sha1(nonce+created.encode()+password.encode()).digest()).decode()
        root='http://docs.oasis-open.org/wss/2004/01/'
        security=f'<w:Security s:mustUnderstand="1" xmlns:w="{WSSE}" xmlns:u="{WSU}"><w:UsernameToken><w:Username>{escape(user)}</w:Username><w:Password Type="{root}oasis-200401-wss-username-token-profile-1.0#PasswordDigest">{digest}</w:Password><w:Nonce EncodingType="{root}oasis-200401-wss-soap-message-security-1.0#Base64Binary">{base64.b64encode(nonce).decode()}</w:Nonce><u:Created>{created}</u:Created></w:UsernameToken></w:Security>'
        if args.clock_only or operation=='GetSystemDateAndTime': security=''
        data=f'<s:Envelope xmlns:s="{soap_namespace}" xmlns:m="{namespace}" xmlns:tt="{SCHEMA}"><s:Header>{security}</s:Header><s:Body><m:{operation}>{parameters}</m:{operation}></s:Body></s:Envelope>'.encode()
        content_type='text/xml; charset=utf-8' if args.soap11 else f'application/soap+xml; charset=utf-8; action="{namespace}/{operation}"'
        request=urllib.request.Request(f'http://{host}:{port}{path}',data=data,headers={'Content-Type':content_type,'SOAPAction':f'"{namespace}/{operation}"'})
        # No redirects and no fallback to cloud/device control protocols.
        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self,*a,**kw): return None
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect()).open(request,timeout=5) as response:
                payload=response.read(1024*1024+1)
        except urllib.error.HTTPError as error:
            fault_data=error.read(65536)
            try:
                fault=ET.fromstring(fault_data)
                report['faultValues']=[node.text for node in fault.iter() if node.tag.endswith(('}Value','}Text','faultcode','faultstring')) and node.text][:8]
            except ET.ParseError: pass
            raise
        if len(payload)>1024*1024 or b'<!DOCTYPE' in payload: raise ValueError('Invalid response')
        tree=ET.fromstring(payload);body=tree.find(f'{{{soap_namespace}}}Body')
        if body is None: body=tree.find(f'{{{SOAP}}}Body')
        if args.clock_only: report['rawClockResponse']=ET.tostring(tree,encoding='unicode')
        if body is None or len(body)!=1 or body[0].tag!=f'{{{namespace}}}{operation}Response': raise ValueError('Fault or missing acknowledgement')
        report['operations'].append(operation)
        return body[0]
    def path(address):
        value=urllib.parse.urlsplit(address).path
        if not value.startswith('/onvif/'): raise ValueError('Invalid service path')
        return value
    try:
        if args.camera_clock:
            clock=call('/onvif/device_service',DEVICE,'GetSystemDateAndTime')
            utc=clock.find(f'{{{DEVICE}}}SystemDateAndTime/{{{SCHEMA}}}UTCDateTime')
            def component(parent,name): return int(utc.findtext(f'{{{SCHEMA}}}{parent}/{{{SCHEMA}}}{name}'))
            camera_time=datetime.datetime(component('Date','Year'),component('Date','Month'),component('Date','Day'),component('Time','Hour'),component('Time','Minute'),component('Time','Second'),tzinfo=datetime.timezone.utc)
            clock_offset=camera_time-datetime.datetime.now(datetime.timezone.utc)
            report['clockOffsetSeconds']=round(clock_offset.total_seconds())
        if args.clock_only:
            clock=call('/onvif/device_service',DEVICE,'GetSystemDateAndTime')
            report['clockXml']=ET.tostring(clock,encoding='unicode');report['success']=True
            with args.output.open('x',encoding='utf8') as output: json.dump(report,output,indent=2)
            print(json.dumps(report,indent=2));return 0
        caps=call('/onvif/device_service',DEVICE,'GetCapabilities','<m:Category>All</m:Category>')
        media=path(caps.findtext(f'{{{DEVICE}}}Capabilities/{{{SCHEMA}}}Media/{{{SCHEMA}}}XAddr'))
        ptz=path(caps.findtext(f'{{{DEVICE}}}Capabilities/{{{SCHEMA}}}PTZ/{{{SCHEMA}}}XAddr'))
        profiles=call(media,MEDIA,'GetProfiles')
        report['profilesXml']=ET.tostring(profiles,encoding='unicode')
        configurations=[]
        for profile in profiles:
            config=profile.find(f'{{{SCHEMA}}}PTZConfiguration')
            if config is not None:
                options=call(ptz,PTZ,'GetConfigurationOptions',f'<m:ConfigurationToken>{escape(config.get("token"))}</m:ConfigurationToken>')
                presets=call(ptz,PTZ,'GetPresets',f'<m:ProfileToken>{escape(profile.get("token"))}</m:ProfileToken>')
                configurations.append({'profileToken':profile.get('token'),'optionsXml':ET.tostring(options,encoding='unicode'),'presetCount':len(presets)})
                if args.stationary_move_check:
                    # Zero velocity cannot displace the camera. Never replay a
                    # nonzero movement to negotiate firmware compatibility.
                    report['stationaryMoveCheck']=True
                    report['moveTimeout']=args.move_timeout
                    call(ptz,PTZ,'ContinuousMove',f'<m:ProfileToken>{escape(profile.get("token"))}</m:ProfileToken><m:Velocity><tt:PanTilt x="0" y="0" space="http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"/></m:Velocity><m:Timeout>{args.move_timeout}</m:Timeout>')
                break
        report['profiles']=configurations;report['success']=bool(configurations)
    except Exception as error:
        report['success']=False;report['errorType']=type(error).__name__
        if isinstance(error,urllib.error.HTTPError): report['httpStatus']=error.code
    with args.output.open('x',encoding='utf8') as output: json.dump(report,output,indent=2)
    print(json.dumps(report,indent=2))
    return 0 if report['success'] else 1

if __name__=='__main__': raise SystemExit(main())
