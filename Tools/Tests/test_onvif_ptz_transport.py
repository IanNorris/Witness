"""ONVIF PTZ wire tests against loopback fixtures; never contacts cameras."""
import base64
import hashlib
import os
import subprocess
import threading
import unittest
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from quiet_subprocess import quiet_process_options

SOAP='http://www.w3.org/2003/05/soap-envelope'
DEVICE='http://www.onvif.org/ver10/device/wsdl'
MEDIA='http://www.onvif.org/ver10/media/wsdl'
PTZ='http://www.onvif.org/ver20/ptz/wsdl'
SCHEMA='http://www.onvif.org/ver10/schema'
WSSE='http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd'
WSU='http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd'

@unittest.skipUnless(os.getenv('WITNESS_ONVIF_PTZ_PROBE'), 'Set loopback probe executable')
class OnvifTransportTests(unittest.TestCase):
    def run_probe(self,operation,ack=None,unsupported=False,timeout='PT60S',reject_operation=None):
        calls=[];errors=[];nonces=set()
        test=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_POST(self):
                try:
                    root=ET.fromstring(self.rfile.read(int(self.headers['Content-Length'])))
                    token=root.find(f'.//{{{WSSE}}}UsernameToken')
                    test.assertEqual(token.findtext(f'{{{WSSE}}}Username'),'fixture&user')
                    nonce=base64.b64decode(token.findtext(f'{{{WSSE}}}Nonce'))
                    created=token.findtext(f'{{{WSU}}}Created')
                    expected=base64.b64encode(hashlib.sha1(nonce+created.encode()+b'fixture-password').digest()).decode()
                    test.assertEqual(token.findtext(f'{{{WSSE}}}Password'),expected)
                    test.assertNotIn(nonce,nonces);nonces.add(nonce)
                    body=root.find(f'{{{SOAP}}}Body')[0];ns,op=body.tag[1:].split('}')
                    calls.append((op,self.path,body))
                    if op==reject_operation:
                        self.send_error(400);return
                    if op=='GetCapabilities':
                        payload='' if unsupported else '<t:Media><t:XAddr>http://untrusted.invalid/onvif/media</t:XAddr></t:Media><t:PTZ><t:XAddr>http://untrusted.invalid/onvif/ptz</t:XAddr></t:PTZ>'
                        content=f'<r:GetCapabilitiesResponse xmlns:r="{DEVICE}"><r:Capabilities>{payload}</r:Capabilities></r:GetCapabilitiesResponse>'
                    elif op=='GetProfiles':
                        content=f'<r:GetProfilesResponse xmlns:r="{MEDIA}"><r:Profiles token="main&amp;profile"><t:PTZConfiguration token="ptz-config"/></r:Profiles></r:GetProfilesResponse>'
                    elif op=='GetConfigurationOptions':
                        content=f'<r:GetConfigurationOptionsResponse xmlns:r="{PTZ}"><r:PTZConfigurationOptions><t:Spaces><t:ContinuousPanTiltVelocitySpace><t:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</t:URI><t:XRange><t:Min>-1</t:Min><t:Max>1</t:Max></t:XRange><t:YRange><t:Min>-1</t:Min><t:Max>1</t:Max></t:YRange></t:ContinuousPanTiltVelocitySpace></t:Spaces><t:PTZTimeout><t:Min>PT1S</t:Min><t:Max>{timeout}</t:Max></t:PTZTimeout></r:PTZConfigurationOptions></r:GetConfigurationOptionsResponse>'
                    elif op=='GetPresets':
                        content=f'<r:GetPresetsResponse xmlns:r="{PTZ}"><r:Preset token="saved&amp;1"><t:Name>Gate &amp; drive</t:Name></r:Preset></r:GetPresetsResponse>'
                    else:
                        test.assertEqual(body.findtext(f'{{{PTZ}}}ProfileToken'),'main&profile')
                        content=ack if ack is not None else f'<r:{op}Response xmlns:r="{PTZ}"/>'
                    data=f'<e:Envelope xmlns:e="{SOAP}" xmlns:t="{SCHEMA}"><e:Body>{content}</e:Body></e:Envelope>'.encode()
                    self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
                except BaseException as error:
                    errors.append(error);self.send_error(500)
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            result=subprocess.run([os.environ['WITNESS_ONVIF_PTZ_PROBE'],str(server.server_port),operation],capture_output=True,text=True,timeout=30,**quiet_process_options())
        finally:
            server.shutdown();server.server_close();thread.join()
        if errors: raise errors[0]
        self.last_error=result.stderr
        return result.returncode,calls

    def test_move_and_timeout(self):
        code,calls=self.run_probe('move',timeout='PT5S');self.assertEqual(code,0)
        move=calls[-1][2]
        velocity=move.find(f'{{{PTZ}}}Velocity/{{{SCHEMA}}}PanTilt')
        self.assertEqual(float(velocity.get('x')),-.5);self.assertEqual(float(velocity.get('y')),.5)
        self.assertEqual(move.findtext(f'{{{PTZ}}}Timeout'),'PT5.000000S')
        self.assertEqual(calls[-1][1],'/onvif/ptz')
    def test_stop(self):
        code,calls=self.run_probe('stop');self.assertEqual(code,0)
        self.assertEqual(calls[-1][0],'Stop');self.assertEqual(calls[-1][2].findtext(f'{{{PTZ}}}PanTilt'),'true')
    def test_preset_tokens(self):
        code,calls=self.run_probe('preset');self.assertEqual(code,0)
        self.assertEqual(calls[-1][2].findtext(f'{{{PTZ}}}PresetToken'),'saved&1')
    def test_cancelled_movement_never_reaches_wire(self):
        code,calls=self.run_probe('cancel');self.assertEqual(code,0)
        self.assertNotIn('ContinuousMove',[c[0] for c in calls])
    def test_no_ptz_never_moves(self):
        code,calls=self.run_probe('move',unsupported=True);self.assertEqual(code,3)
        self.assertEqual([c[0] for c in calls],['GetCapabilities'])
    def test_unsafe_timeout_never_moves(self):
        code,calls=self.run_probe('move',timeout='PT0S');self.assertEqual(code,3)
        self.assertNotIn('ContinuousMove',[c[0] for c in calls])
    def test_profile_transport_error_is_not_misreported_as_missing_ptz(self):
        code,calls=self.run_probe('discover',reject_operation='GetProfiles')
        self.assertEqual(code,3);self.assertIn('ONVIF GetProfiles: HTTP 400',self.last_error)
        self.assertNotIn('No ONVIF PTZ media profile',self.last_error)
    def test_ambiguous_acknowledgements_fail(self):
        for ack in ['', '<html/>',f'<r:OtherResponse xmlns:r="{PTZ}"/>', f'<e:Fault/>',f'<r:StopResponse xmlns:r="wrong"/>']:
            with self.subTest(ack=ack):
                code,_=self.run_probe('stop',ack=ack);self.assertEqual(code,3)

if __name__=='__main__': unittest.main()
