import { ZeroConf } from 'capacitor-zeroconf'

export interface MdnsLamp {
  name: string
  ip: string
  port: number
}

export async function scanMdnsLamps(
  onLamp: (lamp: MdnsLamp) => void,
  durationMs = 5_000,
) {
  // watch() takes { type, domain } and an optional callback; returns a CallbackID string.
  // The action 'resolved' indicates a fully-discovered service with address info.
  await ZeroConf.watch(
    { type: '_lamp._tcp.', domain: 'local.' },
    (result) => {
      if (result.action === 'resolved' && result.service.ipv4Addresses?.[0]) {
        onLamp({
          name: result.service.name,
          ip: result.service.ipv4Addresses[0],
          port: result.service.port,
        })
      }
    },
  )
  setTimeout(() => {
    void ZeroConf.unwatch({ type: '_lamp._tcp.', domain: 'local.' })
  }, durationMs)
}
