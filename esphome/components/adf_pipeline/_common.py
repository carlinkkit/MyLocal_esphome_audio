from esphome.util import Registry
import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.core import ID


esp_adf_ns = cg.esphome_ns.namespace("esp_adf")

ADFPipelineController = esp_adf_ns.class_("ADFPipelineController")

ADFPipelineElement = esp_adf_ns.class_("ADFPipelineElement")
ADFPipelineSink = esp_adf_ns.class_("ADFPipelineSinkElement", ADFPipelineElement)
ADFPipelineSource = esp_adf_ns.class_("ADFPipelineSourceElement", ADFPipelineElement)
ADFPipelineProcess = esp_adf_ns.class_("ADFPipelineProcessElement", ADFPipelineElement)


def register_element(name, element_type, schema=None):
    if schema is None:
        schema = []
    return ELEMENT_REGISTRY.register(name, element_type, schema)


async def get_registered_element(conf):
    return await cg.build_registry_entry(ELEMENT_REGISTRY, conf)


ELEMENT_REGISTRY = Registry()
validate_element = cv.validate_registry_entry("element", ELEMENT_REGISTRY)
validate_element_list = cv.validate_registry("element", ELEMENT_REGISTRY)


@register_element("resampler", esp_adf_ns.class_("ADFResampler", ADFPipelineProcess),{
    cv.Optional("src_rate", default=16000): cv.int_,
    cv.Optional("src_num_channels", default=2): cv.int_range(1,2),
    cv.Optional("dst_rate", default=16000): cv.int_,
    cv.Optional("dst_num_channels", default=2): cv.int_range(1,2),
})
def do_resampler(config, element_id):
    cntrl = cg.new_Pvariable(element_id)
    cg.add(cntrl.set_src_rate(config["src_rate"]))
    cg.add(cntrl.set_src_num_channels(config["src_num_channels"]))

    cg.add(cntrl.set_dst_rate(config["dst_rate"]))
    cg.add(cntrl.set_dst_num_channels(config["dst_num_channels"]) )

    return cntrl

