"""General ADF-Pipeline Setup."""

import os

from ._common import ( # noqa: F401
    esp_adf_ns,
    ADFPipelineElement,
    ADFPipelineProcess,
    ADFPipelineSink,
    ADFPipelineSource,
    ADFPipelineController,
    register_element,
    get_registered_element,
    validate_element,
)

import esphome.codegen as cg

from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import coroutine_with_priority, ID


CODEOWNERS = ["@gnumpi"]
DEPENDENCIES = []

IS_PLATFORM_COMPONENT = True

CONF_ADF_COMPONENT_TYPE = "type"
CONF_ADF_PIPELINE = "pipeline"
CONF_ADF_KEEP_PIPELINE_ALIVE = "keep_pipeline_alive"

# Pipeline Controller

COMPONENT_TYPES = ["sink", "source", "filter"]
SELF_DESCRIPTORS = ["this", "source", "sink", "self"]

ADF_PIPELINE_CONTROLLER_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ADF_COMPONENT_TYPE): cv.one_of(*COMPONENT_TYPES),
        cv.Optional(CONF_ADF_KEEP_PIPELINE_ALIVE, default=False): cv.boolean,
        cv.Optional(CONF_ADF_PIPELINE): cv.ensure_list(
            cv.Any(
                cv.one_of(*SELF_DESCRIPTORS),
                validate_element,
                cv.use_id(ADFPipelineElement),
            )
        ),
    }
)


async def setup_pipeline_controller(cntrl, config: dict) -> None:
    """Set controller parameter and register elements to pipeline."""

    cg.add(cntrl.set_keep_alive(config[CONF_ADF_KEEP_PIPELINE_ALIVE]))

    if CONF_ADF_PIPELINE in config:
        for comp_id in config[CONF_ADF_PIPELINE]:
            if isinstance(comp_id, str):
                if comp_id in SELF_DESCRIPTORS:
                    cg.add(cntrl.append_own_elements())
            elif isinstance(comp_id, ID):
                    comp = await cg.get_variable(comp_id)
                    cg.add(cntrl.add_element_to_pipeline(comp))
            else:
                comp = await get_registered_element(comp_id)
                cg.add(cntrl.add_element_to_pipeline(comp))


@coroutine_with_priority(55.0)
async def to_code(config):
    cg.add_define("USE_ESP_ADF_VAD")

    cg.add_platformio_option("build_unflags", "-Wl,--end-group")

    cg.add_platformio_option(
        "board_build.embed_txtfiles", "components/dueros_service/duer_profile"
    )

    esp32.add_idf_sdkconfig_option("CONFIG_ESP_TLS_INSECURE", True)
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", True)

    esp32.add_extra_script(
        "pre",
        "apply_adf_patches.py",
        os.path.join(os.path.dirname(__file__), "apply_adf_patches.py.script"),
    )

    esp32.add_extra_build_file(
        "esp_adf_patches/idf_v4.4_freertos.patch",
        "https://github.com/espressif/esp-adf/raw/v2.5/idf_patches/idf_v4.4_freertos.patch",
    )

    esp32.add_idf_component(
        name="esp-adf",
        repo="https://github.com/espressif/esp-adf.git",
        ref="v2.5",
        path="components",
        submodules=["components/esp-adf-libs", "components/esp-sr"],
        components=[
            "audio_pipeline",
            "audio_sal",
            "esp-adf-libs",
            "esp-sr",
            "dueros_service",
            "clouds",
            "audio_stream",
            "audio_board",
            "esp_peripherals",
            "audio_hal",
            "display_service",
            "esp_dispatcher",
            "esp_actions",
            "wifi_service",
            "audio_recorder",
            "tone_partition",
        ],
    )
