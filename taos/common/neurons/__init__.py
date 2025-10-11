# SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
# SPDX-License-Identifier: MIT
# The MIT License (MIT)
# Copyright © 2023 Yuma Rao
# Copyright © 2025 Rayleigh Research

# Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
# documentation files (the “Software”), to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all copies or substantial portions of
# the Software.

# THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
# THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

import copy
import typing

import bittensor as bt

from abc import ABC, abstractmethod
from threading import Thread

# Sync calls set weights and also resyncs the metagraph.
from taos.common.config import check_config, add_args, config
from taos.common.utils.misc import ttl_get_block
from taos.common.utils.pagerduty import triggerPagerDutyIncident, resolvePagerDutyIncident
from taos import __spec_version__ as spec_version
from taos.mock import MockSubtensor, MockMetagraph
from taos.common.utils.subnet_hyperparameters import get_subnet_hyperparameters


class BaseNeuron(ABC):
    """
    Base class for τaos subnet neurons. This class is abstract and should be inherited by a subclass. It contains the core logic for all neurons; validators and miners.

    In addition to creating a wallet, subtensor, and metagraph, this class also handles the synchronization of the network state via a basic checkpointing mechanism based on epoch length.
    """

    neuron_type: str = "BaseNeuron"

    @classmethod
    def check_config(cls, config: "bt.Config"):
        check_config(cls, config)

    @classmethod
    def add_args(cls, parser):
        add_args(cls, parser)

    @classmethod
    def config(cls):
        return config(cls)

    subtensor: "bt.subtensor"
    wallet: "bt.wallet"
    metagraph: "bt.metagraph"
    spec_version: int = spec_version

    @property
    def block(self):
        return ttl_get_block(self)

    def __init__(self, config=None):
        base_config = copy.deepcopy(config or BaseNeuron.config())
        self.config = self.config()
        self.config.merge(base_config)
        self.check_config(self.config)

        # If a gpu is required, set the device to cuda:N (e.g. cuda:0)
        self.device = self.config.neuron.device

        # Log the configuration for reference.
        bt.logging.info(f"Config:\n{self.config}")

        # Build Bittensor objects
        # These are core Bittensor classes to interact with the network.
        bt.logging.info("Setting up bittensor objects:")

        # The wallet holds the cryptographic key pairs for the miner.
        if self.config.mock:
            self.wallet = bt.MockWallet(config=self.config)
            self.subtensor = MockSubtensor(
                self.config.netuid, wallet=self.wallet
            )
            self.metagraph = MockMetagraph(
                self.config.netuid, subtensor=self.subtensor
            )
        else:
            self.wallet = bt.wallet(
                    path=self.config.wallet.path,
                    name=self.config.wallet.name,
                    hotkey=self.config.wallet.hotkey
                )
            self.subtensor = bt.subtensor(self.config.subtensor.chain_endpoint)
            self.metagraph = self.subtensor.metagraph(self.config.netuid)

        bt.logging.info(f"Wallet: {self.wallet}")
        bt.logging.info(f"Subtensor: {self.subtensor}")
        bt.logging.info(f"Metagraph: {self.metagraph}")

        # Check if the hotkey is registered on the Bittensor network before proceeding further.
        self.check_registered()

        # Each registered hotkey gets a unique identity (UID) in the network for differentiation.
        self.uid = self.metagraph.hotkeys.index(
            self.wallet.hotkey.ss58_address
        )
        # Retrieve the current block number
        self.update_block()
        # Retrieve the current subnet hyperparameters
        self.update_hyperparams()
        bt.logging.info(
            f"Neuron Running! Subnet : {self.config.netuid} | UID : {self.uid} | Endpoint : {self.subtensor.chain_endpoint}"
        )
        self.step = 0

    @abstractmethod
    def run(self):
        """
        Abstract method for running the neuron.
        """
        ...
    

    @abstractmethod
    def resync_metagraph(self):
        """
        Abstract method for resynchronizing metagraph.
        """
        ...

    def set_weights(self):
        """
        Default method for setting validator weights.
        """
        pass

    def sync(self, save_state=True):
        """
        Wrapper for synchronizing the state of the network for the given miner or validator.
        """
        # Ensure miner or validator hotkey is still registered on the network.
        self.check_registered()
        # Update block and hyperparameters
        self.update_block()
        self.update_hyperparams()

        if self.should_sync_metagraph():
            self.resync_metagraph()

        if self.should_set_weights():
            self.set_weights()

        if save_state:
            self.save_state()

    def check_registered(self):
        """
        Method to check if the hotkey configured to be used by the neuron is registered in the subnet.
        """
        bt.logging.debug("Checking registration...")
        if not self.subtensor.is_hotkey_registered(
            netuid=self.config.netuid,
            hotkey_ss58=self.wallet.hotkey.ss58_address,
        ):
            bt.logging.error(
                f"Wallet: {self.wallet} is not registered on netuid {self.config.netuid}."
                f" Please register the hotkey using `btcli subnets register` before trying again"
            )
            exit()        
        bt.logging.debug(f"Key {self.config.wallet.name}.{self.config.wallet.hotkey} ({self.wallet.hotkey.ss58_address}) is registered.")

    def update_block(self, max_retries=5):
        """
        Method to update `self.current_block` with the latest block number from the chain.
        """
        bt.logging.debug("Getting latest block number...")
        retries = 0
        last_exception = None
        while retries < max_retries:
            try:
                self.current_block = self.subtensor.block                
                bt.logging.debug(f"Current Block : {self.current_block}")
                return True
            except Exception as e:
                last_exception = e
                bt.logging.error(f"Failed to retrieve current block : {e}")
                retries += 1
                continue
        raise(last_exception)

    def update_hyperparams(self):
        """
        Method to update `self.hyperparams` with the latest subnet hyperparameters from the chain.
        """
        bt.logging.debug("Updating Subnet Hyperparams...")
        self.hyperparams = get_subnet_hyperparameters(self.subtensor, self.config.netuid)
        bt.logging.debug(f"Subnet Hyperparams:\n{self.hyperparams}")

    def should_sync_metagraph(self):
        """
        Check if enough epoch blocks have elapsed since the last checkpoint to sync.
        """
        return True

    def should_set_weights(self) -> bool:
        """
        Method to check whether weights should be set by the neuron at the current block.
        """
        # Don't set weights on initialization or if disabled in config.
        if self.step == 0 or self.config.neuron.disable_set_weights:
            return False

        last_updated = self.metagraph.last_update[self.uid].item()
        bt.logging.trace(f"Last Update : {last_updated} | Rate Limit : {self.hyperparams.weights_rate_limit} | Current Block : {self.current_block}")
        return (
            last_updated < self.current_block - self.hyperparams.weights_rate_limit # attempt to set weights as soon as weights rate limiting allows
            and self.neuron_type != "MinerNeuron" # don't set weights if you're a miner
        )  

    def save_state(self):
        """
        Method to save (serialize) the state of the neuron (to be implemented by subclasses).
        """
        bt.logging.warning(
            "save_state() not implemented for this neuron."
        )

    def load_state(self):
        """
        Method to load (deserialize) the state of the neuron (to be implemented by subclasses).
        """
        bt.logging.warning(
            "load_state() not implemented for this neuron."
        )

    def pagerduty_alert(self, incident_text : str, method : str = "unknown", dedup_key : str | None = None, details : dict | None = None, event_class : str = "ERROR", severity : str = "error"):
        bt.logging.error("PD: " + incident_text + (f"\nDetails : {details}" if details else ''))
        if self.config.alerting.pagerduty.integration_key:
            triggerPagerDutyIncident(
                integration_keys=[self.config.alerting.pagerduty.integration_key], 
                source=f"{self.config.wallet.name}:{self.config.wallet.hotkey}:{method}", 
                group=f"{self.config.wallet.name}:{self.config.wallet.hotkey}",
                event_class=event_class, 
                msg=f"τaos {self.config.wallet.name}:{self.config.wallet.hotkey} - {method} : {incident_text}",
                custom_details=details, 
                severity=severity,
                dedup_key=dedup_key
            )
        else:
            bt.logging.debug(f"pagerduty_alert : PagerDuty is not configured.")
