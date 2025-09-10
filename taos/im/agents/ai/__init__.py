# SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
# SPDX-License-Identifier: MIT

import os
import ast
import inspect
import bittensor as bt

from typing import Any
from pathlib import Path
from threading import Thread
from abc import abstractmethod
from taos.im.agents import FinanceSimulationAgent
from taos.im.protocol import MarketSimulationStateUpdate, FinanceAgentResponse


class FinanceSimulationAIAgent(FinanceSimulationAgent):
    """
    Base class for AI-based financial simulation agents.

    This agent provides a flexible framework for model-based decision making in a market simulation.
    It supports automated model training, checkpointing, data recording, and response generation.

    Subclasses must implement abstract methods for model initialization, serialization, training,
    and data handling.
    """

    def handle(self, state: MarketSimulationStateUpdate) -> FinanceAgentResponse:
        """
        Handle a new market simulation state update.

        - Updates internal state with the new simulation data.
        - Triggers model training if the training interval has elapsed.
        - Invokes model response generation and logs the result.

        Args:
            state (MarketSimulationStateUpdate): The latest simulation state.

        Returns:
            FinanceAgentResponse: The agent's generated response for the current state.
        """
        self.update(state)
        for book_id, book in state.books.items():
            if book_id in self.last_train_time and int(state.timestamp - self.last_train_time[book_id]) > self.train_interval * 1_000_000_000:
                if self.trained_events[book_id] == 0:
                    self._train(book_id, state.timestamp, test=True)
                    self.update_model(book_id)
                else:
                    Thread(target=self._train, args=(book_id, state.timestamp), kwargs={'test': True}).start()

        response = self.respond(state)
        self.report(state, response)
        return response

    def features_file(self, book_id: int) -> str:
        """
        Returns the path to the CSV file containing recorded feature data for a given book.

        Args:
            book_id (int): ID of the order book.

        Returns:
            str: File path to the features CSV.
        """
        return os.path.join(self.output_dir, f'features.{book_id}.csv')

    def targets_file(self, book_id: int) -> str:
        """
        Returns the path to the CSV file containing target labels for a given book.

        Args:
            book_id (int): ID of the order book.

        Returns:
            str: File path to the targets CSV.
        """
        return os.path.join(self.output_dir, f'targets.{book_id}.csv')

    def checkpoint_file(self, book_id: int) -> str:
        """
        Returns the full path to the model checkpoint file for a specific book.

        Args:
            book_id (int): ID of the order book.

        Returns:
            str: File path to the model checkpoint.
        """
        return os.path.join(self.checkpoint_dir, f"{self.model}.{book_id}")

    @abstractmethod
    def init_model(self, book_id: int):
        """
        Abstract method to initialize a new model instance for a given book.

        Must be implemented by subclasses.

        Args:
            book_id (int): ID of the order book.
        """
        ...

    @abstractmethod
    def load_model(self, checkpoint: str):
        """
        Abstract method to load a model from a checkpoint.

        Must be implemented by subclasses.

        Args:
            checkpoint (str): Path to the saved model checkpoint.
        """
        ...

    @abstractmethod
    def save_model(self, location: str):
        """
        Abstract method to save the model to the specified location.

        Must be implemented by subclasses.

        Args:
            location (str): Path to save the model.
        """
        ...

    @abstractmethod
    def record_data(self, book_id: int, data: Any):
        """
        Abstract method to record training data (features and targets).

        Must be implemented by subclasses.

        Args:
            book_id (int): ID of the order book.
            data (Any): Data to record.
        """
        ...

    def update_model(self, book_id: int):
        """
        Update the in-memory model from a newly saved checkpoint, if available.

        If a new checkpoint (with `.new` suffix) exists, it replaces the old one.
        """
        try:
            new_path = self.checkpoint_file(book_id) + '.new'
            if os.path.exists(new_path):
                self.models[book_id] = self.load_model(new_path)
                if os.path.exists(self.checkpoint_file(book_id)):
                    try:
                        os.remove(self.checkpoint_file(book_id))
                        bt.logging.debug(f"Removed old checkpoint for book {book_id}")
                    except Exception as e:
                        bt.logging.error(f"Failed to remove old checkpoint: {e}")
                os.rename(new_path, self.checkpoint_file(book_id))
            else:
                bt.logging.debug(f"No new model found for book {book_id}")
        except Exception as ex:
            bt.logging.error(f"Exception while updating model: {ex}")

    @abstractmethod
    def pretrain(self):
        """
        Abstract method for pretraining logic, to be executed at startup if enabled.

        Must be implemented by subclasses.
        """
        ...

    @abstractmethod
    def train(self, book_id: int, timestamp: int, test: bool = False) -> bool:
        """
        Abstract method for training the model on recent data.

        Must be implemented by subclasses.

        Args:
            book_id (int): ID of the order book.
            timestamp (int): Simulation timestamp of training.
            test (bool): If True, runs in test mode.

        Returns:
            bool: True if training succeeded.
        """
        ...

    def _train(self, book_id: int, timestamp: int, test: bool = False):
        """
        Internal wrapper for model training that also updates training state.

        Args:
            book_id (int): ID of the order book.
            timestamp (int): Simulation timestamp.
            test (bool): Run training in test mode.
        """
        success = self.train(book_id, timestamp, test)
        if success:
            self.last_train_time[book_id] = timestamp
            self.trained_events[book_id] += 1
            self.model_trained[book_id] = self.model_trained[book_id] or (self.trained_events[book_id] >= self.min_train_events)

    def get_module_parameters(self, module):
        """
        Parse and populate model parameters from self.config based on the module's constructor.

        Uses reflection and literal evaluation to populate self.model_kwargs.
        """
        signature = inspect.signature(self.module.__init__)
        parameters = signature.parameters
        for name, param in parameters.items():
            if name != 'self' and (
                param.kind == inspect.Parameter.KEYWORD_ONLY or
                param.kind == inspect.Parameter.POSITIONAL_OR_KEYWORD
            ):
                if hasattr(self.config, name):
                    attrib = getattr(self.config, name)
                    try:
                        self.model_kwargs[name] = ast.literal_eval(attrib)
                    except:
                        self.model_kwargs[name] = attrib
                else:
                    self.model_kwargs[name] = param.default

    def print_config(self):
        """
        Print the agent’s configuration and model parameters to the log.
        """
        bt.logging.info(f"""
---------------------------------------------------------------
Strategy Config
---------------------------------------------------------------
Model : {self.model}
Model Parameters :
""" + '\n'.join([f"\t{name} : {val}" for name, val in self.model_kwargs.items()]) + f"""
Checkpoint : {self.checkpoint if self.checkpoint else 'N/A'}
Pretraining : {self.should_pretrain}
Sampling Interval : {self.sampling_interval}s
Training Observations : {self.train_n}
Training Interval : {self.train_interval}
Minimum Training Requirement : {self.min_train_events} runs
Output Directory : {self.output_dir}
---------------------------------------------------------------""")

    def prepare(self, model: str):
        """
        Prepare the agent by configuring directories, parameters, and optionally pretraining.

        Args:
            model (str): Model name used for identification and file naming.
        """
        self.model = model

        self.checkpoint_dir = self.config.checkpoint_dir if hasattr(self.config, 'checkpoint_dir') else os.path.join(self.output_dir, 'checkpoints',f'{self.model}')
        Path(self.checkpoint_dir).mkdir(parents=True, exist_ok=True)
        self.checkpoint = self.config.checkpoint if hasattr(self.config, 'checkpoint') else None

        self.reset = bool(self.config.reset) if hasattr(self.config,'reset') else False
        
        # Set the time window for the features in simulation seconds, if no value specified this defaults to 1 second
        self.sampling_interval = self.config.sampling_interval if hasattr(self.config, 'sampling_interval') else 1
        # Set the number of observations to use in training, if no value specified this defaults to 60 observations
        self.train_n = int(self.config.train_n) if hasattr(self.config,'train_n') else 60
        # Set the interval at which training should be executed in simulation seconds, if no value specified this defaults to 1 simulation minute
        self.train_interval = self.config.train_interval if hasattr(self.config, 'train_interval') else 60
        # Set the number of times training must be completed before beginning inference, if no value specified this defaults to 1 training execution
        self.min_train_events = self.config.min_train_events if hasattr(self.config, 'min_train_events') else 3
        
        self.model_trained = {}
        self.last_train_time = {}
        self.trained_events = {}

        # If you have already collected data in previous runs and would like to train a new model on the existing data,
        # set `pretrain=1` in the launch command to execute training on all recorded data when the agent is started
        self.should_pretrain = bool(self.config.pretrain) if hasattr(self.config,'pretrain') else False
        self.print_config()
            
        self.models = {}
        
        if self.should_pretrain:
            self.pretrain()
                    
    def init_book(self, book_id : int) -> None:
        """
        Initialize model utilized in response processing for the specified book
        """
        if not self.reset:
            if self.checkpoint:
                bt.logging.info(f'Loading checkpoint {self.checkpoint}')
                self.models[book_id] = self.load_model(os.path.join(self.checkpoint_dir, self.checkpoint))
                self.model_trained[book_id] = True
                self.trained_events[book_id] = self.min_train_events
                self.last_train_time[book_id] = 0
            else:
                if os.path.exists(self.checkpoint_file(book_id)):
                    bt.logging.info(f'Loading model {self.checkpoint_file(book_id)} for book {book_id}')
                    self.models[book_id] = self.load_model(self.checkpoint_file(book_id))
                    self.model_trained[book_id] = True
                    self.trained_events[book_id] = self.min_train_events
                    self.last_train_time[book_id] = 0
                else:
                    bt.logging.info(f'No {self.model} checkpoint for book {book_id} - initializing new model.')
                    self.models[book_id] = self.init_model(book_id)
                    self.model_trained[book_id] = False
                    self.trained_events[book_id] = 0
                    self.last_train_time[book_id] = 0
        else:
            self.models[book_id] = self.init_model(book_id)
            self.model_trained[book_id] = False
            self.trained_events[book_id] = 0
            self.last_train_time[book_id] = 0
