import asyncio
import logging
from reolink_aio.api import Host

# Enable debug logging to see Baichuan protocol details
logging.basicConfig(level=logging.DEBUG)

async def test():
    # Test HTTP API first
    host = Host('camfrontptz.home.norris.at', 'admin', '9Pd1Z1qYqHMxxy*NrdKGc', port=443)
    try:
        print('=== HTTP API ===')
        await host.get_host_data()
        print(f'Model: {host.model}, HW: {host.hardware_version}, FW: {host.sw_version}')
        print(f'Channels: {host.channels}')
        
        # Try subscribing to events (this uses Baichuan TCP on port 9000)
        print('\n=== Baichuan TCP (port 9000) ===')
        print('Subscribing to push notifications...')
        await host.subscribe(sub_type='push')
        print('Subscribe succeeded!')
        
        # Wait a bit for events
        await asyncio.sleep(5)
        print('Done waiting for events')
        
    except Exception as e:
        print(f'Error: {type(e).__name__}: {e}')
        import traceback
        traceback.print_exc()
    finally:
        await host.unsubscribe_all()
        await host.logout()

asyncio.run(test())
