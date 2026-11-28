import copy

import pytest

from run_quality_revalidation import Quality, method_order


def test_every_method_occupies_each_launch_position_across_seeds():
    configs = ['full', 'half', 'rr']
    rounds = [method_order(configs, i) for i in range(3)]
    assert all(set(r) == set(configs) for r in rounds)
    assert all({r[i] for r in rounds} == set(configs) for i in range(3))
    assert configs == ['full', 'half', 'rr']


@pytest.mark.parametrize('upscaler', ['DLSS', 'DLSS-RR'])
def test_fixed_inputs_and_retained_history_reject_corruption(upscaler):
    runner = Quality.__new__(Quality)
    config = {'name': 'test', 'upscaler': upscaler, 'accumulates': True}
    states = [{'seed': '17', 'accumulated_frames': str(i+1),
               'upscaler_history_reset': str(int(i == 0)),
               'frame_time_ms': str(1000/60),
               'dlss_exposure': str(int(upscaler == 'DLSS'))} for i in range(3)]
    runner.validate_states(states, config, 17)
    for field, wrong in [('frame_time_ms', '33.3333'),
                         ('dlss_exposure', str(int(upscaler != 'DLSS'))),
                         ('accumulated_frames', '99'), ('upscaler_history_reset', '1')]:
        bad = copy.deepcopy(states)
        bad[1][field] = wrong
        with pytest.raises((AssertionError, RuntimeError)):
            runner.validate_states(bad, config, 17)
