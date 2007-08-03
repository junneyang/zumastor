#!/bin/bash

. config_replication

ssh $source_uml_host "zumastor forget volume $vol"
ssh $target_uml_host "zumastor forget volume $vol"

ssh $source_uml_host "halt"
ssh $target_uml_host "halt"
