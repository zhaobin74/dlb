#!/bin/bash

### Default Vars ###

FLAVOR=OMPSS
POLICY=LeWI_mask
TRACING=NO
MPI_WAIT_MODE=BLOCK
THREAD_DISTRIB=NO
CPUS_NODE=16   # --> FIXME parametrize from configure 
SHOW=NO
KEEP=NO
EXTRAE_CFG=$EXTRAE_CONFIG_FILE
DEBUG=NO


### PATHS ###
DLB_PATH=/home/bsc15/bsc15994/MN3/dlb/install/lib  # --> FIXME parametrize from configure 
SMPSS_PATH=/home/bsc15/bsc15994/SMPSs-install${bits}/lib  # --> FIXME parametrize from configure 
TRACE_PATH=/home/bsc15/bsc15994/MN3/extrae/lib  # --> FIXME parametrize from configure 

##################
### Print Help ###
##################
function help
{
   SC=`basename $0`
   echo "Syntax: $SC [OPTIONS] -- APP [APP_ARGS]"
   echo ""
   echo "OPTIONS:"
   echo "  --debug                  : Use debug version of DLB [Default = $DEBUG]"
   echo "  --extrae-cfg x           : Use this extrae config file for tracing [Default EXTRAE_CONFIG_FILE env var = $EXTRAE_CONFIG_FILE]"
   echo "  --flavor x               : Application progamming model [Default = $FLAVOR]"
   echo "         MPI_ONLY             : MPI only application"
   echo "         OMPSS                : OMPSs application"
   echo "         OMP                  : OMP application, not in Nanos runtime"
   echo "         OMP-NANOS            : OMP application in Nanos runtime"
   echo "         SMPSS                : SMPSs application"
   echo "  --keep                   : Keep generated script [Default = $KEEP]"
   echo "  --mpi-wait-mode x        : MPI wwait mode when in a MPI blocking call [Default = $MPI_WAIT_MODE]"
   echo "         BLOCK                : Blocking wait mode, not consuimg cpu"
   echo "         1CPU                 : Polling wait mode, consuming cpu"
   echo "  --num-mpis-node x           : Number of MPI processes running in one node. Mandatory for OMP and SMPSS flavor "
   echo "  --policy x               : DLB policy [Default = $POLICY]"
   echo "         ORIG                 : Do not use DLB library"
   echo "         NO                   : Do not load balance, use DLB just for profile"
   echo "         LeWI                 : Lend When Idle policy"
   echo "         LeWI_mask            : Lend When Idle and use cpu binding policy (Only available for OMPSs or OMP-NANOS flavor)"
   echo "         RaL:                 : Redistribute and Lend policy with cpu binding (Only available for OMPSs or OMP-NANOS flavor)"
   echo "  --smpss-cfg x               : Path to SMPSs configuration file"
   echo "  --show                   : Do not run application but show output of this script [Default = $SHOW]"
   echo "  --thread-distribution x  : Use an heterogeneous distribution of threads among MPI processes [Default = $THREAD_DISTRIB]"
   echo "         NO                   : Use homogeneous thread distribution among MPI processes"
   echo "         x-y-z-w              : Numbers separated by '-'. First MPI x threads, second MPI y threads..."
   echo "  --tracing                : Trace application using Extrae [Default = $TRACING]"
   echo ""
   echo " Following environment variables are honored:"
   echo "      EXTRAE_HOME=$EXTRAE_HOME"
   echo "      EXTRAE_CONFIG_FILE=$EXTRAE_CONFIG_FILE"
   echo "      EXTRAE_LABELS=$EXTRAE_LABELS"
   echo "      EXTRAE_FINAL_DIR=$EXTRAE_FINAL_DIR"
   exit -1
}


###################
### Parse flags ###
###################

while [ "$1" != "--" ]; do
   case "$1" in
      --flavor)
         FLAVOR="$2"
         shift;
         ;;
      --mpi-wait-mode)
         MPI_WAIT_MODE=$2
         shift;
         ;;
      --show)
         SHOW=YES
         ;;
      --keep)
         KEEP=YES
         ;;
      --tracing)
         TRACING=YES
         ;;
      --extrae-cfg)
         EXTRAE_CFG=$2
         shift;
         ;;
      --policy)
         POLICY=$2
         shift;
         ;;
      --thread-distribution)
         THREAD_DISTRIB=$2
         shift;
         ;;
      --debug)
         DEBUG=YES
         ;;
      --help|-h)
         help
         ;;
      --smpss-cfg)
         SMPSS_CFG=$2
         shift;
         ;;
      --num-mpis-node)
         MPIS_NODE=$2
         shift;
         ;;
   *)
      echo "$0: Not valid argument [$1]";
      help
      ;;
   esac
   shift;
done

shift
APP=$1
shift
APP_ARGS=$@


SCRIPT=dlb_script.sh
echo "#!/bin/bash" > $SCRIPT


########################
### flavor selection ###
########################

FLAVOR=$(echo $FLAVOR | tr "[:lower:]" "[:upper:]")

if [ "$FLAVOR" == "MPI_ONLY" ]
then
	EXTRAE_LIB=libmpitrace

elif [ "$FLAVOR" == "SMPSS" ]
then
	EXTRAE_LIB=libsmpssmpitrace

elif [ "$FLAVOR" == "OMP" ]
then
	EXTRAE_LIB=libompitrace

elif [ "$FLAVOR" == "OMPSS" -o "$FLAVOR" == "OMP-NANOS" ]
then
	EXTRAE_LIB=libnanosmpitrace

else
	echo "[Set DLB]: ERROR: Unknown FLAVOR: $FLAVOR"
	exit -1
fi
echo "[Set DLB]: Using $FLAVOR flavor"


##############################
### Deciding blocking mode ###
##############################

echo "" >> $SCRIPT
MPI_WAIT_MODE=$(echo $MPI_WAIT_MODE | tr "[:lower:]" "[:upper:]")

if [ "${MPI_WAIT_MODE}" == "BLOCK" ]
then
   echo "# MPI set to blocking mode" >> $SCRIPT
   ### FIXME another solution in the future? ###
   
   ### MPICH ###
   echo "export MXMPI_RECV=blocking #MPICH" >> $SCRIPT
   
   ### OpenMPI ###
   echo "export OMPI_MCA_mpi_yield_when_idle=1 #OpenMPI" >> $SCRIPT
   
   ### Intel MPI ###
   echo "export I_MPI_MPI_WAIT_MODE=1 #Intel MPI" >> $SCRIPT

   echo "" >> $SCRIPT
   echo "# DLB env vars" >> $SCRIPT
   echo "export LB_LEND_MODE=BLOCK" >> $SCRIPT
   echo "[Set DLB]: Using MPI wait mode blocking"
   
elif [ "${MPI_WAIT_MODE}" == "1CPU" ]
then
   echo "# DLB env vars" >> $SCRIPT
   echo "export LB_LEND_MODE=1CPU " >> $SCRIPT
   echo "[Set DLB]: Using MPI wait mode 1 CPU"
else
   echo "[Set DLB]: ERROR: Unknown blocking mode ${MPI_WAIT_MODE}"
   exit -1
fi


####################
### DLB ENV VARS ###
####################

echo "export LB_POLICY=$POLICY" >> $SCRIPT
echo "[Set DLB]: Using $POLICY policy"


###########################
### Thread distribution ###
###########################

if [ $THREAD_DISTRIB != "NO" ]
then
   echo "export LB_THREAD_DISTRIBUTION=$THREAD_DISTRIB" >> $SCRIPT
fi


#######################
### Extrae env vars ###
#######################

if [ "$TRACING" == "YES" ]
then
   echo "[Set DLB]: Using extrae tracing"
   echo "" >> $SCRIPT
   echo "# Extrae env vars" >> $SCRIPT

   NANOS+=" --instrumentation=extrae"

   if [ "${EXTRAE_CFG}" == "" ]
   then
      echo "export EXTRAE_ON=1 " >> $SCRIPT
   else
      echo "export EXTRAE_CONFIG_FILE=${EXTRAE_CFG} " >> $SCRIPT
   fi
fi


#######################
### NANOS arguments ###
#######################

if [ "$FLAVOR" == "OMPSS" -o "$FLAVOR" == "OMP-NANOS" ]
then
   if [ "$POLICY" == "LeWI" ]
   then
      NANOS= " --disable-binding"
      ###--instrument-cpuid"
   fi

   if [ "$POLICY" != "ORIG" ]
   then
      NANOS+=" --enable-dlb"
   fi

   echo "" >> $SCRIPT
   echo "# Nanos arguments" >> $SCRIPT
   echo "export NX_ARGS+=\"$NANOS\"" >> $SCRIPT

elif [ "$POLICY" == "LeWI_mask" -o "$POLICY" == "RaL" ]
then
   echo "[Set DLB]: ERROR: Incompatible flavor and policy: flavor=$FLAVOR policy=$POLICY"
   exit -1
   
fi

if [[ ( "$FLAVOR" == "SMPSs" || "$FLAVOR" == "OMP" ) &&  "$MPIS_NODE" == "" ]]
then
   echo "[Set DLB]: ERROR: --num-mpis-node is a mandatory flag when using flavor $FLAVOR"
   exit -1
else
   CPUS_PROC=$(($CPUS_NODE/$MPIS_NODE))
fi

######################
### SMPSs env vars ###
######################

if [ "$FLAVOR" == "SMPSs" ]
then
   echo "" >> $SCRIPT
   echo "# SMPSs env vars " >> $SCRIPT

   echo "eexport CSS_NUM_CPUS=$CPUS_PROC" >> $SCRIPT
   echo "export CSS_MAX_CPUS=$CPUS_NODE" >> $SCRIPT
   echo "export CSS_CONFIG_FILE=${SMPSS_CFG}" >> $SCRIPT
fi


######################
### OpenMP env vars ###
######################

if [ "$FLAVOR" == "OMP" ]
then
   echo "" >> $SCRIPT
   echo "# OpenMP env vars " >> $SCRIPT
   echo "export OMP_NUM_THREADS=$CPUS_PROC" >> $SCRIPT
fi

####################
### Library path ###
### Preload      ###
####################

echo "" >> $SCRIPT
echo "# Library path and preload" >> $SCRIPT
echo "export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$DLB_PATH " >> $SCRIPT


if [ "$POLICY" == "ORIG" ]
then
   if [ "$TRACING" == "YES" ]
   then
      EXTRAE_LIB=${TRACE_PATH}/${EXTRAE_LIB}.so
      if [ -f $EXTRAE_LIB ]
      then
         echo "export LD_PRELOAD=${EXTRAE_LIB}" >> $SCRIPT
      else
         echo "[Set DLB]: ERROR: Extrae lib does not exist: $EXTRAE_LIB"
         exit -1
      fi
   fi
else

   if [ "$TRACING" == "YES" ]
   then
      EXTRAE_LIB=${TRACE_PATH}/${EXTRAE_LIB}-lb.so
      if [ -f $EXTRAE_LIB ]
      then
         if [ "$DEBUG" == "YES" ]
         then
            echo "export LD_PRELOAD=${EXTRAE_LIB}:${DLB_PATH}/libdlb_instr_dbg.so" >> $SCRIPT
         else
            echo "export LD_PRELOAD=${EXTRAE_LIB}:${DLB_PATH}/libdlb_instr.so" >> $SCRIPT
         fi
      else
         echo "[Set DLB]: ERROR: Extrae lib does not exist: $EXTRAE_LIB. Check the path and that Extrae was installed with DLB support"
         exit -1
      fi
   else
      if [ "$DEBUG" == "YES" ]
      then
         echo "export LD_PRELOAD=${DLB_PATH}/libdlb_dbg.so" >> $SCRIPT
      else
         echo "export LD_PRELOAD=${DLB_PATH}/libdlb.so" >> $SCRIPT
      fi
   fi
fi 


########################
### Finishing script ###
########################

echo "" >> $SCRIPT
echo "# Finally run application with its parameters" >> $SCRIPT
echo "\$*" >> $SCRIPT

if [ "$SHOW" == "YES" ]
then
   cat $SCRIPT
else
   chmod +x $SCRIPT

   $SCRIPT $APP $APP_ARGS
fi

if [ "$KEEP" != "YES" ]
then
   rm -f $SCRIPT
fi


   
#================================================================================================================================================
#================================================================================================================================================
#================================================================================================================================================
#================================================================================================================================================




