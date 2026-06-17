import { describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'

import ExpressionConfig from '../expressions/ExpressionConfig.vue'

const configSchema = {
  colors: {
    min: 1,
    max: 5,
  },
}

describe('ExpressionConfig', () => {
  it('renders the new candle controls', () => {
    const wrapper = mount(ExpressionConfig, {
      props: {
        expression: {
          type: 'candle',
          enabled: true,
          colors: ['#FF932960'],
          intervalMin: 300,
          intervalMax: 1200,
          target: 2,
          duration: 30,
          flickerAmount: 18,
        },
        configSchema,
      },
    })

    expect(wrapper.text()).toContain('Run Time')
    expect(wrapper.text()).toContain('Flicker Amount')
  })

  it('renders the new comet controls', () => {
    const wrapper = mount(ExpressionConfig, {
      props: {
        expression: {
          type: 'comet',
          enabled: true,
          colors: ['#FFF0D0AA'],
          intervalMin: 180,
          intervalMax: 600,
          target: 2,
          cometSpeed: 8,
          cometTail: 8,
        },
        configSchema,
      },
    })

    expect(wrapper.text()).toContain('Travel Time')
    expect(wrapper.text()).toContain('Tail Length')
  })
})
