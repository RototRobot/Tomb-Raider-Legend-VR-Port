"""Writes the SteamVR action manifest and default bindings for the OpenVR
backend (src/vr/ovr_runtime.cpp, 1.1.0).

Since 1.1.0 vr_input.cpp turns physical controls into game actions for both
backends, so SteamVR gets one action per physical control and hand, bound
one to one. Players can still rebind them in SteamVR's bindings screen.

    python tools/gen_openvr_bindings.py

Writes input/actions.json (installed as trlvr_actions.json) and
input/bindings_<controller>.json.
"""
import json
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OUT = os.path.join(ROOT, 'input')
SIDES = ('left', 'right')

BOOLEANS = ('squeeze', 'stick_click', 'primary', 'secondary', 'trackpad',
            'menu', 'trigger_touch', 'thumb_touch')
OPTIONAL = {'trackpad', 'menu', 'trigger_touch', 'thumb_touch', 'primary',
            'secondary'}

CONTROLLERS = {
    'hpmotioncontroller': {
        'desc': 'HP Reverb G2 controllers',
        'buttons': {'left': {'x': 'primary', 'y': 'secondary'},
                    'right': {'a': 'primary', 'b': 'secondary'}},
    },
    'oculus_touch': {
        'desc': 'Oculus / Meta Touch controllers',
        'buttons': {'left': {'x': 'primary', 'y': 'secondary',
                             'application_menu': 'menu'},
                    'right': {'a': 'primary', 'b': 'secondary'}},
        'touch': True,
    },
    'knuckles': {
        'desc': 'Valve Index controllers',
        'buttons': {'left': {'a': 'primary', 'b': 'secondary',
                             'trackpad': 'trackpad'},
                    'right': {'a': 'primary', 'b': 'secondary',
                              'trackpad': 'trackpad'}},
        'touch': True,
    },
    'holographic_controller': {
        'desc': 'Windows Mixed Reality controllers',
        'buttons': {'left': {'trackpad': 'trackpad',
                             'application_menu': 'menu'},
                    'right': {'trackpad': 'trackpad',
                              'application_menu': 'menu'}},
    },
}


def action(name):
    return '/actions/trlvr/in/' + name


def manifest():
    actions = []
    for side in SIDES:
        actions.append({'name': action('stick_' + side), 'type': 'vector2'})
        actions.append({'name': action('trigger_' + side), 'type': 'vector1'})
        for b in BOOLEANS:
            a = {'name': action('%s_%s' % (b, side)), 'type': 'boolean'}
            if b in OPTIONAL:
                a['requirement'] = 'optional'
            actions.append(a)
        actions.append({'name': action('hand_' + side), 'type': 'pose'})
        # SteamVR's OpenXR grip pose, for comparison in the log (1.1.0).
        actions.append({'name': action('hand_xr_' + side), 'type': 'pose',
                        'requirement': 'optional'})
        actions.append({'name': '/actions/trlvr/out/haptic_' + side,
                        'type': 'vibration', 'requirement': 'optional'})
    return {
        'default_bindings': [
            {'controller_type': c, 'binding_url': 'bindings_%s.json' % c}
            for c in CONTROLLERS],
        'actions': actions,
        'action_sets': [{'name': '/actions/trlvr', 'usage': 'single'}],
        'localization': [{
            'language_tag': 'en_US',
            '/actions/trlvr': 'Tomb Raider: Legend VR',
            **{a['name']: a['name'].rsplit('/', 1)[1].replace('_', ' ')
               for a in actions},
        }],
    }


def bindings(controller, spec):
    sources, poses, haptics = [], [], []
    touch = spec.get('touch', False)
    for side in SIDES:
        hand = '/user/hand/' + side
        stick = {'position': {'output': action('stick_' + side)},
                 'click': {'output': action('stick_click_' + side)}}
        if touch:
            stick['touch'] = {'output': action('thumb_touch_' + side)}
        sources.append({'path': hand + '/input/joystick', 'mode': 'joystick',
                        'inputs': stick})
        trigger = {'pull': {'output': action('trigger_' + side)}}
        if touch:
            trigger['touch'] = {'output': action('trigger_touch_' + side)}
        sources.append({'path': hand + '/input/trigger', 'mode': 'trigger',
                        'inputs': trigger})
        sources.append({'path': hand + '/input/grip', 'mode': 'button',
                        'inputs': {'click': {'output': action('squeeze_' + side)}}})
        for component, name in spec['buttons'][side].items():
            inputs = {'click': {'output': action('%s_%s' % (name, side))}}
            if touch and component != 'application_menu':
                inputs['touch'] = {'output': action('thumb_touch_' + side)}
            sources.append({'path': '%s/input/%s' % (hand, component),
                            'mode': 'button', 'inputs': inputs})
        poses.append({'output': action('hand_' + side),
                      'path': hand + '/pose/grip'})
        poses.append({'output': action('hand_xr_' + side),
                      'path': hand + '/pose/openxr_grip'})
        haptics.append({'output': '/actions/trlvr/out/haptic_' + side,
                        'path': hand + '/output/haptic'})
    return {
        'controller_type': controller,
        'description': 'Tomb Raider: Legend VR -- ' + spec['desc'],
        'name': 'Tomb Raider: Legend VR',
        'bindings': {'/actions/trlvr': {'sources': sources, 'poses': poses,
                                        'haptics': haptics}},
    }


def write(name, data):
    path = os.path.join(OUT, name)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=2)
        f.write('\n')
    print('wrote', os.path.relpath(path, ROOT))


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    write('actions.json', manifest())
    for c, spec in CONTROLLERS.items():
        write('bindings_%s.json' % c, bindings(c, spec))
