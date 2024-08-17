from esphome.util import Registry
import esphome.config_validation as cv
import esphome.codegen as cg

esp_adf_ns = cg.esphome_ns.namespace("esp_adf")

ADFPipelineController = esp_adf_ns.class_("ADFPipelineController")

ADFPipelineElement = esp_adf_ns.class_("ADFPipelineElement")
ADFPipelineSink = esp_adf_ns.class_("ADFPipelineSinkElement", ADFPipelineElement)
ADFPipelineSource = esp_adf_ns.class_("ADFPipelineSourceElement", ADFPipelineElement)
ADFPipelineProcess = esp_adf_ns.class_("ADFPipelineProcessElement", ADFPipelineElement)

def register_element(name, element_type):
    return ELEMENT_REGISTRY.register(name, element_type, {})

def get_registered_element (name):
    element = ELEMENT_REGISTRY[name]
    if element == None:
        return None
    return element.type_id

ELEMENT_REGISTRY = Registry()
validate_element = cv.validate_registry_entry("element", ELEMENT_REGISTRY)
validate_element_list = cv.validate_registry("element", ELEMENT_REGISTRY)


register_element("resampler", esp_adf_ns.class_("ADFResampler", ADFPipelineProcess, ADFPipelineElement))
