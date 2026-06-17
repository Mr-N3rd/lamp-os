import { describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'

import ExpressionsList from '../expressions/ExpressionsList.vue'

describe('ExpressionsList', () => {
  it('shows the curated lightweight effects in the add modal', async () => {
    const wrapper = mount(ExpressionsList, {
      props: {
        modelValue: [],
      },
    })

    await wrapper.find('.add-button').trigger('click')

    expect(wrapper.text()).toContain('Palette Cycle')
    expect(wrapper.text()).toContain('Soft Twinkle')
    expect(wrapper.text()).toContain('Gentle Comet')
    expect(wrapper.text()).toContain('Candle')
  })

  it('builds new candle expressions from schema defaults', async () => {
    const wrapper = mount(ExpressionsList, {
      props: {
        modelValue: [],
      },
    })

    await wrapper.find('.add-button').trigger('click')
    const candleButton = wrapper
      .findAll('.expression-type-button')
      .find((button) => button.text().includes('Candle'))

    expect(candleButton).toBeTruthy()
    await candleButton!.trigger('click')

    const emitted = wrapper.emitted('update:modelValue')
    expect(emitted).toBeTruthy()

    const expressions = emitted![0][0] as Array<Record<string, unknown>>
    expect(expressions).toHaveLength(1)
    expect(expressions[0]).toMatchObject({
      type: 'candle',
      enabled: true,
      target: 2,
      duration: 30,
      flickerAmount: 18,
    })
    expect(expressions[0].colors).toEqual(['#FF932960', '#FFB34740'])
  })
})
