#
# Out of range parameters
#
echo "####################################################"
echo "# Test out of range parameters and error conditions."
echo "####################################################"


#
# Select a large file from the list
#
tstf=
tsz=0
for tf in `cat files.lst`
do
	sz=`ls -l ${tf} | awk '{ print $5 }'`
	if [ $sz -gt $tsz ]
	then
		tsz=$sz
		tstf="$tf"
	fi
done

export PCOMPRESS_CHUNK_HASH_GLOBAL=CRC64
cmd="../../pcompress -G -c none -l1 -s10m $tstf"
echo "Running $cmd"
eval $cmd
if [ $? -eq 0 ]
then
	echo "FATAL: Compression DID NOT ERROR where expected"
	rm -f ${tstf}.pz
fi
unset PCOMPRESS_CHUNK_HASH_GLOBAL
if [ -f core* ]
then
	echo "FATAL: Compression crashed"
	rm -f ${tstf}.pz
	rm -f core*
fi

for feat in "-L" "-L -D" "-L -D -E -M -C" "-L -B5" "-L -D -E -B2" "-F" "-F -L"
do
	cmd="../../pcompress -c dummy -l4 -s1m $feat $tstf"
	echo "Running $cmd"
	eval $cmd
	if [ $? -eq 0 ]
	then
		echo "FATAL: Compression DID NOT ERROR where expected"
		rm -f ${tstf}.pz
		break
	fi
	if [ -f core* ]
	then
		echo "FATAL: Compression crashed"
		rm -f ${tstf}.pz
		rm -f core*
		break
	fi
	rm -f ${tstf}.pz
done

for feat in "-B8 -s2m -l1" "-B-1 -s2m -l1" "-D -s10k -l1" "-D -F -s2m -l1" "-p -e AES -s2m -l1" "-s2m -l15" "-e AES -k64" "-e SALSA20 -k8" "-e AES -k8" "-e SALSA20 -k64"
do
	for algo in lzfx lz4 zlib bzip2 libbsc ppmd lzma
	do
		cmd="../../pcompress -c lzfx $feat $tstf"
		echo "Running $cmd"
		eval $cmd
		if [ $? -eq 0 ]
		then
			echo "FATAL: Compression DID NOT ERROR where expected"
			rm -f ${tstf}.pz
			break
		fi
		rm -f ${tstf}.pz
	done
done

#
# Create a file larger than 2GB
#
if [ ! -f ${tstf}.1 ]
then
	echo -n "Creating a file approximately 2GB in size "
	cp ${tstf} ${tstf}.1
	t_sz=$tsz
	while [ $t_sz -lt 2147480000 ]
	do
		cat ${tstf} >> ${tstf}.1
		echo -n "."
		t_sz=$((t_sz + tsz))
	done
	echo ""
	echo "Done"
fi

#
# Try to compress with segment sizes larger than what some compression algos allow.
#
for feat in "-E -M -C -s 2147480000" "-D -E -M -C -L -P -B 2 -s 2147480000"
do
	for algo in lz4 libbsc
	do
		cmd="../../pcompress -c $algo $feat ${tstf}.1"
		echo "Running $cmd"
		eval $cmd
		if [ $? -eq 0 ]
		then
			echo "FATAL: Compression DID NOT ERROR where expected"
			rm -f ${tstf}.1.pz
			break
		fi
		rm -f ${tstf}.1.pz
	done
done
rm -f ${tstf}.1.pz
rm -f ${tstf}.1

for feat in "-S CRC64" "-S BLAKE256" "-S BLAKE512" "-S SHA256" "-S SHA512" "-S KECCAK256" "-S KECCAK512"
do
	rm -f ${tstf}.*

	cmd="../../pcompress -c lzfx -l3 -s1m $feat ${tstf}"
	echo "Running $cmd"
	eval $cmd
	if [ $? -ne 0 ]
	then
		echo "FATAL: Compression errored."
		rm -f ${tstf}.pz
		d=`dirname ${tstf}`
		rm -f ${d}/.pc*
		continue
	fi

	echo "Corrupting file header ..."
	dd if=/dev/urandom conv=notrunc of=${tstf}.pz bs=4 seek=1 count=1
	cmd="../../pcompress -d ${tstf}.pz ${tstf}.1"
	eval $cmd
	if [ $? -eq 0 ]
	then
		echo "FATAL: Decompression DID NOT ERROR where expected."
	fi

	rm -f ${tstf}.pz
	rm -f ${tstf}.1

	cmd="../../pcompress -c zlib -l3 -s1m $feat ${tstf}"
	echo "Running $cmd"
	eval $cmd
	if [ $? -ne 0 ]
	then
		echo "FATAL: Compression errored."
		rm -f ${tstf}.pz ${tstf}.1
		continue
	fi

	cp ${tstf}.pz ${tstf}.1.pz
	echo "Corrupting file ..."
	dd if=/dev/urandom conv=notrunc of=${tstf}.pz bs=4 seek=100 count=1
	cmd="../../pcompress -d ${tstf}.pz ${tstf}.1"
	eval $cmd
	if [ $? -eq 0 ]
	then
		echo "FATAL: Decompression DID NOT ERROR where expected."
		rm -f ${tstf}.pz
	fi

	rm -f ${tstf}.1
	cp ${tstf}.1.pz ${tstf}.pz
	echo "Corrupting file ..."
	dd if=/dev/urandom conv=notrunc of=${tstf}.1.pz bs=4 seek=51 count=1
	cmd="../../pcompress -d ${tstf}.1.pz ${tstf}.1"
	eval $cmd
	if [ $? -eq 0 ]
	then
		echo "FATAL: Decompression DID NOT ERROR where expected."
		rm -f ${tstf}.pz
		rm -f ${tstf}.1
		rm -f ${tstf}.1.pz
	fi

	rm -f ${tstf}.1 ${tstf}.1.pz ${tstf}.pz
	echo "plainpass" > /tmp/pwf
	cmd="../../pcompress -c zlib -l3 -s1m -e SALSA20 -w /tmp/pwf $feat ${tstf}"
	echo "Running $cmd"
	eval $cmd
	if [ $? -ne 0 ]
	then
		echo "FATAL: Compression errored."
		rm -f ${tstf}.pz ${tstf}.1
		continue
	fi
	pw=`cat /tmp/pwf`
	if [ "$pw" = "plainpasswd" ]
	then
		echo "FATAL: Password file was not zeroed"
		rm -f /tmp/pwf
	fi

	cp ${tstf}.pz ${tstf}.1.pz
	echo "Corrupting file ..."
	dd if=/dev/urandom conv=notrunc of=${tstf}.pz bs=4 seek=115 count=1
	echo "plainpass" > /tmp/pwf
	cmd="../../pcompress -d -w /tmp/pwf ${tstf}.pz ${tstf}.1"
	eval $cmd
	if [ $? -eq 0 ]
	then
		echo "FATAL: Decompression DID NOT ERROR where expected."
		rm -f ${tstf}.pz
		rm -f ${tstf}.1
	fi

	cp ${tstf}.1.pz ${tstf}.pz
	rm -f ${tstf}.1
	echo "Corrupting file header ..."
	dd if=/dev/urandom conv=notrunc of=${tstf}.pz bs=4 seek=10 count=1
	echo "plainpass" > /tmp/pwf
	cmd="../../pcompress -d -w /tmp/pwf ${tstf}.pz ${tstf}.1"
	eval $cmd
	if [ $? -eq 0 ]
	then
		echo "FATAL: Decompression DID NOT ERROR where expected."
	fi
done

rm -f ${tstf}.1.pz
rm -f ${tstf}.pz
rm -f ${tstf}.1
rm -f /tmp/pwf

echo "#################################################"
echo ""

