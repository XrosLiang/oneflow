import os
import numpy as np
import tensorflow as tf
import oneflow as flow
from collections import OrderedDict

from test_util import GenArgList
from test_util import GetSavePath
from test_util import Save


def compare_with_tensorflow(device_type, x_shape, y_shape, x_is_zero, y_is_zero):
    assert device_type in ["gpu", "cpu"]
    flow.clear_default_session()

    func_config = flow.FunctionConfig()
    func_config.default_data_type(flow.float)
    func_config.train.primary_lr(1e-4)
    func_config.train.model_update_conf(dict(naive_conf={}))

    @flow.function(func_config)
    def XdivyJob():
        with flow.device_prior_placement(device_type, "0:0"):
            if x_is_zero:
                Initializer_x = flow.zeros_initializer()
            else:
                Initializer_x = flow.random_uniform_initializer(minval=1, maxval=5)
            x = flow.get_variable(
                "x",
                shape=x_shape,
                dtype=flow.float,
                initializer=Initializer_x,
                trainable=True,
            )

            if y_is_zero:
                Initializer_y = flow.zeros_initializer()
            else:
                Initializer_y = flow.random_uniform_initializer(minval=1, maxval=5)
            y = flow.get_variable(
                "y",
                shape=y_shape,
                dtype=flow.float,
                initializer=Initializer_y,
                trainable=True,
            )

            loss = flow.math.xdivy(x, y)
            flow.losses.add_loss(loss)

            flow.watch(x, Save("x"))
            flow.watch_diff(x, Save("x_diff"))
            flow.watch(y, Save("y"))
            flow.watch_diff(y, Save("y_diff"))
            flow.watch(loss, Save("loss"))
            flow.watch_diff(loss, Save("loss_diff"))

            return loss

    # OneFlow
    check_point = flow.train.CheckPoint()
    check_point.init()
    of_out = XdivyJob().get()
    # TensorFlow
    with tf.GradientTape(persistent=True) as tape:
        x = tf.Variable(np.load(os.path.join(GetSavePath(), "x.npy")))
        y = tf.Variable(np.load(os.path.join(GetSavePath(), "y.npy")))
        tf_out = tf.math.xdivy(x, y)
    loss_diff = np.load(os.path.join(GetSavePath(), "loss_diff.npy"))
    tf_x_diff = tape.gradient(tf_out, x, loss_diff)
    tf_y_diff = tape.gradient(tf_out, y, loss_diff)

    assert np.allclose(of_out.ndarray(), tf_out.numpy(), rtol=1e-5, atol=1e-5)
    assert np.allclose(np.load(os.path.join(GetSavePath(), "x_diff.npy")), tf_x_diff.numpy(), rtol=1e-5, atol=1e-5)
    assert np.allclose(np.load(os.path.join(GetSavePath(), "y_diff.npy")), tf_y_diff.numpy(), rtol=1e-5, atol=1e-5)


def test_xdivy(test_case):
    arg_dict = OrderedDict()
    arg_dict["device_type"] = ["gpu"]
    arg_dict["x_shape"] = [(5,)]
    arg_dict["y_shape"] = [(5,)]

    arg_dict["x_is_zero"] = [True]
    arg_dict["y_is_zero"] = [True]
    for arg in GenArgList(arg_dict):
        compare_with_tensorflow(*arg)

    arg_dict["x_is_zero"] = [True]
    arg_dict["y_is_zero"] = [False]
    for arg in GenArgList(arg_dict):
        compare_with_tensorflow(*arg)

    arg_dict["x_is_zero"] = [False]
    arg_dict["y_is_zero"] = [True]
    for arg in GenArgList(arg_dict):
        compare_with_tensorflow(*arg)

    arg_dict["x_is_zero"] = [False]
    arg_dict["y_is_zero"] = [False]
    for arg in GenArgList(arg_dict):
        compare_with_tensorflow(*arg)